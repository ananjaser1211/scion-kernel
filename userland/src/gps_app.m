/*
 * HakiosGPS ("GPS" on the home screen, com.hakios.gps) -- the GNSS receiver's live status,
 * for walking around with the phone and watching it: the state and the fix, a sky plot,
 * the signal of every satellite, the position and its accuracy, and Start / Pause.
 *
 * WHERE THE DATA COMES FROM. Nothing here talks to the receiver or to locationd. gnssd
 * (src/gnssd.m) owns the receiver and publishes what it knows in a property list:
 *
 *   /var/mobile/Library/Caches/com.hakios.gnssd/status.plist
 *
 * rewritten atomically about once a second, after which it posts the Darwin notification
 * com.hakios.gnssd.status. This application reads that file when the notification comes
 * (notify_register_dispatch) and once a second on a timer besides, so a lost notification
 * costs at most a second. Keys read (all optional; a missing key shows as a dash):
 *
 *   state        string  off | starting | loading_esw | engine_on | searching | fix | error
 *   error        string  with state = error
 *   fixType      int     0, 2 or 3
 *   satsInView, satsUsed   int
 *   sats         array of dict: system (GPS|GLONASS|Galileo|BeiDou|QZSS), prn int,
 *                cn0 float dB-Hz, elevation int deg, azimuth int deg, used bool
 *   hdop, vdop, pdop        float
 *   hAccuracy, vAccuracy    float m
 *   latitude, longitude     double, only with fixType >= 2
 *   altitude float m, speed float m/s, course float deg, utc string
 *   ttffSeconds  float (optional), receiverOnSeconds float
 *   updated      date    when gnssd wrote it
 *
 * A file that is missing, unreadable, or older than 5 s (by `updated`, or by its mtime
 * when `updated` is absent) shows "gnssd not running" and none of its contents: stale data
 * on the screen of a person walking about would be a position that is not where they are.
 *
 * COMMANDS. Start and Pause post com.hakios.gnssd.start and com.hakios.gnssd.pause. The
 * badge says "Starting..." / "Pausing..." at once and keeps saying it until gnssd's state
 * agrees or 15 s pass. hakiosgps://start and hakiosgps://pause do the same as the buttons,
 * so a job or a test can press them without a finger.
 *
 * THE SCREEN stays awake (idleTimerDisabled) while the receiver runs or a start is pending,
 * and goes back to the system's idle timer on Pause, when the receiver is off, and when the
 * application leaves the foreground.
 *
 * PRIVACY. The position is drawn on the screen and copied to the pasteboard when the Copy
 * button is pressed, and goes nowhere else: it is not logged, not stored, not sent.
 *
 * All UIKit by hand -- no storyboard, no asset catalog -- because this is built on Linux
 * with xnu-clang and the iPhoneOS 16.5 SDK (userland/build.sh). ARC.
 */
#import <UIKit/UIKit.h>
#import <notify.h>
#import <sys/stat.h>

#define GPS_STATUS_PATH   "/var/mobile/Library/Caches/com.hakios.gnssd/status.plist"
#define GPS_NOTE_STATUS   "com.hakios.gnssd.status"
#define GPS_NOTE_START    "com.hakios.gnssd.start"
#define GPS_NOTE_PAUSE    "com.hakios.gnssd.pause"
#define GPS_STALE_SECONDS 5.0
#define GPS_PENDING_SECONDS 15.0

/* ------------------------------------------------------------------------- helpers */

static NSNumber *
gps_num(NSDictionary *d, NSString *k)
{
	id v = d[k];
	return [v isKindOfClass:[NSNumber class]] ? v : nil;
}

static NSString *
gps_str(NSDictionary *d, NSString *k)
{
	id v = d[k];
	return [v isKindOfClass:[NSString class]] ? v : nil;
}

static UIColor *
gps_rgb(unsigned rgb)
{
	return [UIColor colorWithRed:((rgb >> 16) & 0xff) / 255.0
	    green:((rgb >> 8) & 0xff) / 255.0 blue:(rgb & 0xff) / 255.0 alpha:1];
}

/* One colour and one shape for each constellation, so the plot reads in either. */
typedef enum { GPS_SHAPE_CIRCLE, GPS_SHAPE_SQUARE, GPS_SHAPE_DIAMOND, GPS_SHAPE_TRIANGLE,
    GPS_SHAPE_HEX } gps_shape_t;

static const struct gps_system {
	const char *name, *abbr;
	unsigned rgb;
	gps_shape_t shape;
} gps_systems[] = {
	{ "GPS",     "G", 0x4da3ff, GPS_SHAPE_CIRCLE },
	{ "GLONASS", "R", 0xff5c5c, GPS_SHAPE_SQUARE },
	{ "Galileo", "E", 0xb57bff, GPS_SHAPE_DIAMOND },
	{ "BeiDou",  "C", 0xffa53d, GPS_SHAPE_TRIANGLE },
	{ "QZSS",    "J", 0x2fd4c4, GPS_SHAPE_HEX },
};
#define GPS_NSYSTEMS (sizeof(gps_systems) / sizeof(gps_systems[0]))

static const struct gps_system *
gps_system_for(NSString *name)
{
	for (size_t i = 0; i < GPS_NSYSTEMS; i++)
		if (name && [name caseInsensitiveCompare:@(gps_systems[i].name)] == NSOrderedSame)
			return &gps_systems[i];
	static const struct gps_system other = { "?", "?", 0x9a9a9a, GPS_SHAPE_CIRCLE };
	return &other;
}

static UIColor *
gps_cn0_color(double cn0)
{
	if (cn0 >= 40) return gps_rgb(0x34c759);
	if (cn0 >= 30) return gps_rgb(0xa6d63a);
	if (cn0 >= 20) return gps_rgb(0xffcc00);
	return gps_rgb(0xff453a);
}

static void
gps_path_shape(CGContextRef c, gps_shape_t shape, CGPoint p, CGFloat r)
{
	switch (shape) {
	case GPS_SHAPE_SQUARE:
		CGContextAddRect(c, CGRectMake(p.x - r * 0.88, p.y - r * 0.88, r * 1.76, r * 1.76));
		break;
	case GPS_SHAPE_DIAMOND:
		CGContextMoveToPoint(c, p.x, p.y - r * 1.2);
		CGContextAddLineToPoint(c, p.x + r * 1.2, p.y);
		CGContextAddLineToPoint(c, p.x, p.y + r * 1.2);
		CGContextAddLineToPoint(c, p.x - r * 1.2, p.y);
		CGContextClosePath(c);
		break;
	case GPS_SHAPE_TRIANGLE:
		CGContextMoveToPoint(c, p.x, p.y - r * 1.25);
		CGContextAddLineToPoint(c, p.x + r * 1.1, p.y + r * 0.8);
		CGContextAddLineToPoint(c, p.x - r * 1.1, p.y + r * 0.8);
		CGContextClosePath(c);
		break;
	case GPS_SHAPE_HEX:
		for (int i = 0; i < 6; i++) {
			CGFloat a = M_PI / 3 * i;
			CGFloat x = p.x + r * 1.05 * cos(a), y = p.y + r * 1.05 * sin(a);
			if (i == 0) CGContextMoveToPoint(c, x, y);
			else CGContextAddLineToPoint(c, x, y);
		}
		CGContextClosePath(c);
		break;
	default:
		CGContextAddEllipseInRect(c, CGRectMake(p.x - r, p.y - r, 2 * r, 2 * r));
		break;
	}
}

static NSString *
gps_duration(double s)
{
	if (s < 0) s = 0;
	long t = (long)s;
	if (t >= 3600)
		return [NSString stringWithFormat:@"%ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60];
	return [NSString stringWithFormat:@"%ld:%02ld", t / 60, t % 60];
}

/* Satellites sorted the same way everywhere: by constellation, then PRN. */
static NSArray *
gps_sorted_sats(NSArray *sats)
{
	NSMutableArray *ok = [NSMutableArray array];
	for (id s in sats)
		if ([s isKindOfClass:[NSDictionary class]])
			[ok addObject:s];
	return [ok sortedArrayUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
		const struct gps_system *sa = gps_system_for(gps_str(a, @"system"));
		const struct gps_system *sb = gps_system_for(gps_str(b, @"system"));
		if (sa != sb) return sa < sb ? NSOrderedAscending : NSOrderedDescending;
		return [gps_num(a, @"prn") ?: @0 compare:gps_num(b, @"prn") ?: @0];
	}];
}

/* -------------------------------------------------------------------- the sky plot */

@interface GPSSkyView : UIView
@property (nonatomic, copy) NSArray *sats;
@end

@implementation GPSSkyView

- (instancetype)initWithFrame:(CGRect)f
{
	if ((self = [super initWithFrame:f])) {
		self.backgroundColor = [UIColor clearColor];
		self.contentMode = UIViewContentModeRedraw;
	}
	return self;
}

- (void)setSats:(NSArray *)sats
{
	_sats = [gps_sorted_sats(sats) copy];
	[self setNeedsDisplay];
}

static void
gps_text(NSString *s, CGPoint centre, UIFont *font, UIColor *color)
{
	NSDictionary *a = @{ NSFontAttributeName: font, NSForegroundColorAttributeName: color };
	CGSize z = [s sizeWithAttributes:a];
	[s drawAtPoint:CGPointMake(centre.x - z.width / 2, centre.y - z.height / 2) withAttributes:a];
}

- (void)drawRect:(CGRect)rect
{
	(void)rect;
	CGContextRef c = UIGraphicsGetCurrentContext();
	CGRect b = self.bounds;
	CGFloat R = MIN(b.size.width, b.size.height) / 2 - 22;
	CGPoint o = CGPointMake(CGRectGetMidX(b), CGRectGetMidY(b));
	UIColor *grid = [UIColor secondaryLabelColor];

	/* horizon disc, rings at elevation 0, 30, 60, and the cross */
	CGContextSetFillColorWithColor(c, [UIColor secondarySystemBackgroundColor].CGColor);
	CGContextFillEllipseInRect(c, CGRectMake(o.x - R, o.y - R, 2 * R, 2 * R));
	CGContextSetStrokeColorWithColor(c, [grid colorWithAlphaComponent:0.6].CGColor);
	for (int el = 0; el < 90; el += 30) {
		CGFloat r = R * (90 - el) / 90.0;
		CGContextSetLineWidth(c, el == 0 ? 1.5 : 1);
		CGContextStrokeEllipseInRect(c, CGRectMake(o.x - r, o.y - r, 2 * r, 2 * r));
	}
	CGContextSetLineWidth(c, 0.75);
	CGContextMoveToPoint(c, o.x - R, o.y); CGContextAddLineToPoint(c, o.x + R, o.y);
	CGContextMoveToPoint(c, o.x, o.y - R); CGContextAddLineToPoint(c, o.x, o.y + R);
	CGContextStrokePath(c);

	UIFont *small = [UIFont systemFontOfSize:10];
	gps_text(@"30°", CGPointMake(o.x + 12, o.y - R * 60 / 90.0 + 7), small, grid);
	gps_text(@"60°", CGPointMake(o.x + 12, o.y - R * 30 / 90.0 + 7), small, grid);
	UIFont *card = [UIFont boldSystemFontOfSize:14];
	UIColor *label = [UIColor labelColor];
	gps_text(@"N", CGPointMake(o.x, o.y - R - 11), card, label);
	gps_text(@"S", CGPointMake(o.x, o.y + R + 11), card, label);
	gps_text(@"E", CGPointMake(o.x + R + 11, o.y), card, label);
	gps_text(@"W", CGPointMake(o.x - R - 11, o.y), card, label);

	if (self.sats.count == 0) {
		gps_text(@"no satellites", o, [UIFont systemFontOfSize:13], grid);
		return;
	}
	UIFont *prnFont = [UIFont monospacedDigitSystemFontOfSize:10 weight:UIFontWeightSemibold];
	for (NSDictionary *s in self.sats) {
		NSNumber *el = gps_num(s, @"elevation"), *az = gps_num(s, @"azimuth");
		if (!el || !az || el.doubleValue < 0)
			continue;          /* not placed yet: it still has a bar below */
		const struct gps_system *sys = gps_system_for(gps_str(s, @"system"));
		double r = R * (90 - MIN(90, el.doubleValue)) / 90.0;
		double a = az.doubleValue * M_PI / 180.0;
		CGPoint p = CGPointMake(o.x + r * sin(a), o.y - r * cos(a));
		UIColor *col = gps_rgb(sys->rgb);
		BOOL used = [gps_num(s, @"used") boolValue];
		gps_path_shape(c, sys->shape, p, 7);
		if (used) {
			CGContextSetFillColorWithColor(c, col.CGColor);
			CGContextFillPath(c);
		} else {
			CGContextSetFillColorWithColor(c, [col colorWithAlphaComponent:0.15].CGColor);
			CGContextSetStrokeColorWithColor(c, col.CGColor);
			CGContextSetLineWidth(c, 1.75);
			CGContextDrawPath(c, kCGPathFillStroke);
		}
		NSString *prn = [NSString stringWithFormat:@"%@", gps_num(s, @"prn") ?: @"?"];
		gps_text(prn, CGPointMake(p.x + 15, p.y - 9), prnFont, label);
	}
}

@end

/* --------------------------------------------------------------- the signal chart */

@interface GPSBarsView : UIView
@property (nonatomic, copy) NSArray *sats;
@end

@implementation GPSBarsView

- (instancetype)initWithFrame:(CGRect)f
{
	if ((self = [super initWithFrame:f])) {
		self.backgroundColor = [UIColor clearColor];
		self.contentMode = UIViewContentModeRedraw;
	}
	return self;
}

- (void)setSats:(NSArray *)sats
{
	_sats = [gps_sorted_sats(sats) copy];
	[self setNeedsDisplay];
}

- (void)drawRect:(CGRect)rect
{
	(void)rect;
	CGContextRef c = UIGraphicsGetCurrentContext();
	CGRect b = self.bounds;
	const CGFloat left = 26, top = 16, bottom = 30;
	CGFloat h = b.size.height - top - bottom, w = b.size.width - left - 4;
	UIColor *grid = [UIColor secondaryLabelColor];
	UIFont *small = [UIFont monospacedDigitSystemFontOfSize:9 weight:UIFontWeightRegular];

	/* the scale: 0..50 dB-Hz, a line every 10 */
	CGContextSetLineWidth(c, 0.5);
	for (int db = 0; db <= 50; db += 10) {
		CGFloat y = top + h - h * db / 50.0;
		CGContextSetStrokeColorWithColor(c, [grid colorWithAlphaComponent:db ? 0.35 : 0.8].CGColor);
		CGContextMoveToPoint(c, left, y);
		CGContextAddLineToPoint(c, left + w, y);
		CGContextStrokePath(c);
		gps_text([NSString stringWithFormat:@"%d", db], CGPointMake(left - 12, y), small, grid);
	}
	if (self.sats.count == 0) {
		gps_text(@"no signals", CGPointMake(left + w / 2, top + h / 2),
		    [UIFont systemFontOfSize:13], grid);
		return;
	}
	NSUInteger n = self.sats.count;
	CGFloat slot = w / n, bw = MAX(3, MIN(26, slot * 0.72));
	UIFont *lab = [UIFont monospacedDigitSystemFontOfSize:(slot >= 16 ? 9 : 7)
	    weight:UIFontWeightMedium];
	UIColor *label = [UIColor labelColor];
	for (NSUInteger i = 0; i < n; i++) {
		NSDictionary *s = self.sats[i];
		const struct gps_system *sys = gps_system_for(gps_str(s, @"system"));
		double cn0 = MAX(0, MIN(50, [gps_num(s, @"cn0") doubleValue]));
		BOOL used = [gps_num(s, @"used") boolValue];
		CGFloat x = left + slot * i + (slot - bw) / 2;
		CGFloat bh = h * cn0 / 50.0;
		CGRect bar = CGRectMake(x, top + h - bh, bw, bh);
		UIColor *col = gps_cn0_color(cn0);
		if (used) {
			CGContextSetFillColorWithColor(c, col.CGColor);
			CGContextFillRect(c, bar);
		} else {
			CGContextSetFillColorWithColor(c, [col colorWithAlphaComponent:0.25].CGColor);
			CGContextFillRect(c, bar);
			CGContextSetStrokeColorWithColor(c, col.CGColor);
			CGContextSetLineWidth(c, 1);
			CGContextStrokeRect(c, CGRectInset(bar, 0.5, 0.5));
		}
		CGFloat cx = x + bw / 2;
		if (slot >= 14)
			gps_text([NSString stringWithFormat:@"%.0f", cn0],
			    CGPointMake(cx, top + h - bh - 7), small, grid);
		/* constellation letter in its own colour, the PRN under it */
		gps_text(@(sys->abbr), CGPointMake(cx, top + h + 8), lab, gps_rgb(sys->rgb));
		gps_text([NSString stringWithFormat:@"%@", gps_num(s, @"prn") ?: @"?"],
		    CGPointMake(cx, top + h + 20), lab, label);
	}
}

@end

/* ------------------------------------------------------------- the view controller */

@interface GPSController : UIViewController
- (void)refresh;
- (void)sendStart;
- (void)sendPause;
- (void)leaveForeground;
- (void)enterForeground;
@end

@implementation GPSController {
	UIScrollView *_scroll;
	UIStackView *_stack;
	UILabel *_badge, *_subtitle, *_counts;
	UIView *_staleCard;
	UILabel *_staleText;
	NSArray *_dataViews;
	GPSSkyView *_sky;
	GPSBarsView *_bars;
	UILabel *_legend;
	NSMutableDictionary *_values;   /* detail key -> value label */
	UIButton *_copyButton, *_startButton, *_pauseButton;
	NSString *_latlonText;           /* only ever on the screen or the pasteboard */
	NSString *_pending;              /* @"start" / @"pause" / nil */
	NSDate *_pendingSince;
	NSTimer *_timer;
	int _notifyToken;
	BOOL _keepAwake, _active;
}

- (UILabel *)label:(CGFloat)size weight:(UIFontWeight)wt color:(UIColor *)col
{
	UILabel *l = [[UILabel alloc] init];
	l.font = [UIFont systemFontOfSize:size weight:wt];
	l.textColor = col;
	l.numberOfLines = 0;
	l.adjustsFontSizeToFitWidth = YES;
	l.minimumScaleFactor = 0.6;
	return l;
}

- (UIView *)card:(NSString *)title content:(UIView *)content
{
	UIView *card = [[UIView alloc] init];
	card.backgroundColor = [UIColor secondarySystemBackgroundColor];
	card.layer.cornerRadius = 14;
	UIStackView *v = [[UIStackView alloc] init];
	v.axis = UILayoutConstraintAxisVertical;
	v.spacing = 8;
	v.translatesAutoresizingMaskIntoConstraints = NO;
	if (title) {
		UILabel *t = [self label:13 weight:UIFontWeightSemibold color:[UIColor secondaryLabelColor]];
		t.text = title.uppercaseString;
		[v addArrangedSubview:t];
	}
	[v addArrangedSubview:content];
	[card addSubview:v];
	[NSLayoutConstraint activateConstraints:@[
		[v.topAnchor constraintEqualToAnchor:card.topAnchor constant:12],
		[v.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-12],
		[v.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:14],
		[v.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-14],
	]];
	return card;
}

- (UIButton *)bigButton:(NSString *)title color:(UIColor *)col action:(SEL)sel
{
	UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
	[b setTitle:title forState:UIControlStateNormal];
	b.titleLabel.font = [UIFont systemFontOfSize:22 weight:UIFontWeightBold];
	[b setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
	b.backgroundColor = col;
	b.layer.cornerRadius = 14;
	[b.heightAnchor constraintEqualToConstant:60].active = YES;
	[b addTarget:self action:sel forControlEvents:UIControlEventTouchUpInside];
	return b;
}

- (UIView *)detailsGrid
{
	_values = [NSMutableDictionary dictionary];
	UIStackView *rows = [[UIStackView alloc] init];
	rows.axis = UILayoutConstraintAxisVertical;
	rows.spacing = 6;
	NSArray *keys = @[ @"Latitude", @"Longitude", @"Altitude", @"Accuracy (h / v)",
	    @"HDOP / VDOP / PDOP", @"Speed", @"Course", @"UTC" ];
	for (NSString *k in keys) {
		UIStackView *row = [[UIStackView alloc] init];
		row.axis = UILayoutConstraintAxisHorizontal;
		row.spacing = 8;
		UILabel *name = [self label:15 weight:UIFontWeightRegular color:[UIColor secondaryLabelColor]];
		name.text = k;
		name.numberOfLines = 1;
		UILabel *val = [self label:15 weight:UIFontWeightMedium color:[UIColor labelColor]];
		val.font = [UIFont monospacedDigitSystemFontOfSize:15 weight:UIFontWeightMedium];
		val.textAlignment = NSTextAlignmentRight;
		val.numberOfLines = 1;
		val.text = @"—";
		[name setContentHuggingPriority:UILayoutPriorityRequired
		    forAxis:UILayoutConstraintAxisHorizontal];
		[row addArrangedSubview:name];
		[row addArrangedSubview:val];
		[rows addArrangedSubview:row];
		_values[k] = val;
	}
	_copyButton = [UIButton buttonWithType:UIButtonTypeSystem];
	[_copyButton setTitle:@"Copy position" forState:UIControlStateNormal];
	_copyButton.titleLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
	_copyButton.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeft;
	[_copyButton addTarget:self action:@selector(copyPosition)
	    forControlEvents:UIControlEventTouchUpInside];
	[rows addArrangedSubview:_copyButton];
	return rows;
}

- (NSAttributedString *)legendText
{
	NSMutableAttributedString *s = [[NSMutableAttributedString alloc] init];
	NSArray *glyphs = @[ @"●", @"■", @"◆", @"▲", @"⬢" ];
	UIFont *f = [UIFont systemFontOfSize:12];
	for (size_t i = 0; i < GPS_NSYSTEMS; i++) {
		[s appendAttributedString:[[NSAttributedString alloc]
		    initWithString:[NSString stringWithFormat:@"%@ ", glyphs[i]]
		    attributes:@{ NSFontAttributeName: f,
			NSForegroundColorAttributeName: gps_rgb(gps_systems[i].rgb) }]];
		[s appendAttributedString:[[NSAttributedString alloc]
		    initWithString:[NSString stringWithFormat:@"%s%s", gps_systems[i].name,
			i + 1 < GPS_NSYSTEMS ? "   " : ""]
		    attributes:@{ NSFontAttributeName: f,
			NSForegroundColorAttributeName: [UIColor secondaryLabelColor] }]];
	}
	[s appendAttributedString:[[NSAttributedString alloc]
	    initWithString:@"\nfilled = used in the fix, hollow = in view only"
	    attributes:@{ NSFontAttributeName: f,
		NSForegroundColorAttributeName: [UIColor tertiaryLabelColor] }]];
	return s;
}

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.view.backgroundColor = [UIColor systemBackgroundColor];

	/* header: the badge, the times, the counts */
	_badge = [self label:30 weight:UIFontWeightHeavy color:[UIColor whiteColor]];
	_badge.textAlignment = NSTextAlignmentCenter;
	_badge.numberOfLines = 2;
	_badge.layer.cornerRadius = 14;
	_badge.layer.masksToBounds = YES;
	[_badge.heightAnchor constraintGreaterThanOrEqualToConstant:64].active = YES;
	_subtitle = [self label:16 weight:UIFontWeightMedium color:[UIColor labelColor]];
	_subtitle.font = [UIFont monospacedDigitSystemFontOfSize:16 weight:UIFontWeightMedium];
	_subtitle.textAlignment = NSTextAlignmentCenter;
	_counts = [self label:20 weight:UIFontWeightSemibold color:[UIColor labelColor]];
	_counts.font = [UIFont monospacedDigitSystemFontOfSize:20 weight:UIFontWeightSemibold];
	_counts.textAlignment = NSTextAlignmentCenter;

	/* what shows instead of everything else when gnssd is silent */
	_staleText = [self label:16 weight:UIFontWeightRegular color:[UIColor labelColor]];
	_staleCard = [self card:@"No status from gnssd" content:_staleText];

	_sky = [[GPSSkyView alloc] init];
	[_sky.heightAnchor constraintEqualToAnchor:_sky.widthAnchor].active = YES;
	_legend = [self label:12 weight:UIFontWeightRegular color:[UIColor secondaryLabelColor]];
	_legend.attributedText = [self legendText];
	_legend.textAlignment = NSTextAlignmentCenter;
	UIStackView *skyStack = [[UIStackView alloc] initWithArrangedSubviews:@[ _sky, _legend ]];
	skyStack.axis = UILayoutConstraintAxisVertical;
	skyStack.spacing = 6;

	_bars = [[GPSBarsView alloc] init];
	[_bars.heightAnchor constraintEqualToConstant:190].active = YES;

	UIView *skyCard = [self card:@"Sky" content:skyStack];
	UIView *barsCard = [self card:@"Signal  C/N0 dB-Hz" content:_bars];
	UIView *detailsCard = [self card:@"Position" content:[self detailsGrid]];
	_dataViews = @[ skyCard, barsCard, detailsCard ];

	_stack = [[UIStackView alloc] initWithArrangedSubviews:@[ _badge, _subtitle, _counts,
	    _staleCard, skyCard, barsCard, detailsCard ]];
	_stack.axis = UILayoutConstraintAxisVertical;
	_stack.spacing = 12;
	[_stack setCustomSpacing:4 afterView:_subtitle];
	_stack.translatesAutoresizingMaskIntoConstraints = NO;

	_scroll = [[UIScrollView alloc] init];
	_scroll.translatesAutoresizingMaskIntoConstraints = NO;
	_scroll.alwaysBounceVertical = YES;
	[_scroll addSubview:_stack];
	[self.view addSubview:_scroll];

	/* Start and Pause, outside the scroll view so that they are always under a thumb */
	_startButton = [self bigButton:@"Start" color:gps_rgb(0x2a9d4b) action:@selector(sendStart)];
	_pauseButton = [self bigButton:@"Pause" color:gps_rgb(0xd9822b) action:@selector(sendPause)];
	UIStackView *buttons = [[UIStackView alloc] initWithArrangedSubviews:@[ _startButton,
	    _pauseButton ]];
	buttons.axis = UILayoutConstraintAxisHorizontal;
	buttons.distribution = UIStackViewDistributionFillEqually;
	buttons.spacing = 12;
	buttons.translatesAutoresizingMaskIntoConstraints = NO;
	[self.view addSubview:buttons];

	UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
	[NSLayoutConstraint activateConstraints:@[
		[_scroll.topAnchor constraintEqualToAnchor:safe.topAnchor],
		[_scroll.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
		[_scroll.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
		[_scroll.bottomAnchor constraintEqualToAnchor:buttons.topAnchor constant:-8],
		[_stack.topAnchor constraintEqualToAnchor:_scroll.contentLayoutGuide.topAnchor constant:12],
		[_stack.bottomAnchor constraintEqualToAnchor:_scroll.contentLayoutGuide.bottomAnchor constant:-12],
		[_stack.leadingAnchor constraintEqualToAnchor:_scroll.frameLayoutGuide.leadingAnchor constant:16],
		[_stack.trailingAnchor constraintEqualToAnchor:_scroll.frameLayoutGuide.trailingAnchor constant:-16],
		[buttons.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:16],
		[buttons.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16],
		[buttons.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-12],
	]];

	__weak GPSController *weakSelf = self;
	_notifyToken = NOTIFY_TOKEN_INVALID;
	notify_register_dispatch(GPS_NOTE_STATUS, &_notifyToken, dispatch_get_main_queue(),
	    ^(int token) { (void)token; [weakSelf refresh]; });
	_timer = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES
	    block:^(NSTimer *t) { (void)t; [weakSelf refresh]; }];
	_timer.tolerance = 0.1;
	_active = YES;
	[self refresh];
}

- (void)dealloc
{
	[_timer invalidate];
	if (_notifyToken != NOTIFY_TOKEN_INVALID)
		notify_cancel(_notifyToken);
}

/* ---- commands */

- (void)sendStart
{
	notify_post(GPS_NOTE_START);
	_pending = @"start";
	_pendingSince = [NSDate date];
	[self refresh];
}

- (void)sendPause
{
	notify_post(GPS_NOTE_PAUSE);
	_pending = @"pause";
	_pendingSince = [NSDate date];
	[self refresh];
}

- (void)copyPosition
{
	if (!_latlonText)
		return;
	[UIPasteboard generalPasteboard].string = _latlonText;
	[_copyButton setTitle:@"Copied" forState:UIControlStateNormal];
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)),
	    dispatch_get_main_queue(), ^{
		[self->_copyButton setTitle:@"Copy position" forState:UIControlStateNormal];
	});
}

/* ---- the screen's idle timer */

- (void)setKeepAwake:(BOOL)on
{
	if (on == _keepAwake)
		return;
	_keepAwake = on;
	[UIApplication sharedApplication].idleTimerDisabled = on;
}

- (void)leaveForeground
{
	_active = NO;
	[self setKeepAwake:NO];
}

- (void)enterForeground
{
	_active = YES;
	[self refresh];
}

/* ---- reading gnssd's file */

- (NSDictionary *)readStatus:(double *)ageOut
{
	struct stat st;
	*ageOut = -1;
	if (stat(GPS_STATUS_PATH, &st) != 0)
		return nil;
	NSDictionary *d = [NSDictionary dictionaryWithContentsOfFile:@GPS_STATUS_PATH];
	if (![d isKindOfClass:[NSDictionary class]])
		return nil;
	id up = d[@"updated"];
	NSDate *when = [up isKindOfClass:[NSDate class]] ? up :
	    [NSDate dateWithTimeIntervalSince1970:st.st_mtimespec.tv_sec];
	*ageOut = -[when timeIntervalSinceNow];
	return d;
}

static NSString *
gps_fmt(NSNumber *n, NSString *fmt)
{
	return n ? [NSString stringWithFormat:fmt, n.doubleValue] : @"—";
}

- (void)refresh
{
	double age;
	NSDictionary *d = [self readStatus:&age];
	BOOL fresh = d && age <= GPS_STALE_SECONDS;
	NSString *state = fresh ? (gps_str(d, @"state") ?: @"?") : nil;

	/* a pending Start / Pause ends when gnssd agrees, or after a while */
	if (_pending && -[_pendingSince timeIntervalSinceNow] > GPS_PENDING_SECONDS)
		_pending = nil;
	if (_pending && fresh) {
		BOOL off = [state isEqualToString:@"off"] || [state isEqualToString:@"error"];
		if ([_pending isEqualToString:@"start"] && !off &&
		    ![state isEqualToString:@"starting"])
			_pending = nil;     /* past "starting": its own words take over */
		else if ([_pending isEqualToString:@"pause"] && off)
			_pending = nil;
	}

	NSString *badge, *sub = @"", *counts = @"";
	unsigned colour;
	if (!fresh) {
		badge = @"gnssd not running";
		colour = 0x8e5a12;
		if (!d)
			_staleText.text = [NSString stringWithFormat:@"%s is missing or unreadable.\n\n"
			    "Start gnssd (launchctl kickstart system/com.hakios.gnssd); this screen "
			    "updates by itself once it writes.", GPS_STATUS_PATH];
		else
			_staleText.text = [NSString stringWithFormat:@"The last status is %@ old "
			    "(more than %.0f s), so it is not shown.\n\ngnssd has stopped writing; "
			    "this screen updates by itself once it writes again.",
			    gps_duration(age), GPS_STALE_SECONDS];
		if ([_pending isEqualToString:@"start"])
			sub = @"Start sent, but nothing is answering";
		else if ([_pending isEqualToString:@"pause"])
			sub = @"Pause sent";
	} else {
		int fix = [gps_num(d, @"fixType") intValue];
		if ([_pending isEqualToString:@"pause"]) {
			badge = @"Pausing…"; colour = 0x6b6b6b;
		} else if ([_pending isEqualToString:@"start"]) {
			badge = @"Starting…"; colour = 0x2f6fb5;
		} else if ([state isEqualToString:@"off"]) {
			badge = @"Receiver off"; colour = 0x5a5a5e;
		} else if ([state isEqualToString:@"error"]) {
			badge = [NSString stringWithFormat:@"Error: %@", gps_str(d, @"error") ?: @"unknown"];
			colour = 0xc0392b;
		} else if ([state isEqualToString:@"starting"]) {
			badge = @"Starting…"; colour = 0x2f6fb5;
		} else if ([state isEqualToString:@"loading_esw"]) {
			badge = @"Starting… (firmware)"; colour = 0x2f6fb5;
		} else if (fix >= 3) {
			badge = @"3D fix"; colour = 0x1f8f3e;
		} else if (fix == 2) {
			badge = @"2D fix"; colour = 0xb08900;
		} else if ([state isEqualToString:@"searching"] ||
		    [state isEqualToString:@"engine_on"]) {
			badge = @"Searching…"; colour = 0x2f6fb5;
		} else {
			badge = @"No fix"; colour = 0x7a3fa0;
		}
		NSNumber *on = gps_num(d, @"receiverOnSeconds"), *ttff = gps_num(d, @"ttffSeconds");
		NSMutableArray *parts = [NSMutableArray array];
		if (on && ![state isEqualToString:@"off"])
			[parts addObject:[NSString stringWithFormat:@"On %@", gps_duration(on.doubleValue)]];
		if (ttff)
			[parts addObject:[NSString stringWithFormat:@"TTFF %.1f s", ttff.doubleValue]];
		else if (on && ![state isEqualToString:@"off"])
			[parts addObject:@"TTFF —"];
		sub = [parts componentsJoinedByString:@"   ·   "];
		counts = [NSString stringWithFormat:@"%@ used / %@ in view",
		    gps_num(d, @"satsUsed") ?: @"—", gps_num(d, @"satsInView") ?: @"—"];
	}
	_badge.text = badge;
	_badge.backgroundColor = gps_rgb(colour);
	_subtitle.text = sub;
	_subtitle.hidden = sub.length == 0;
	_counts.text = counts;
	_counts.hidden = counts.length == 0;
	_staleCard.hidden = fresh;
	for (UIView *v in _dataViews)
		v.hidden = !fresh;

	NSArray *sats = fresh && [d[@"sats"] isKindOfClass:[NSArray class]] ? d[@"sats"] : @[];
	_sky.sats = sats;
	_bars.sats = sats;
	[self fillDetails:fresh ? d : nil];

	/* awake while the receiver runs (or is being started), never in the background */
	BOOL running = fresh && ![state isEqualToString:@"off"] && ![state isEqualToString:@"error"];
	BOOL awake = _active && ![_pending isEqualToString:@"pause"] &&
	    (running || [_pending isEqualToString:@"start"]);
	[self setKeepAwake:awake];
}

- (void)fillDetails:(NSDictionary *)d
{
	int fix = [gps_num(d, @"fixType") intValue];
	NSNumber *lat = fix >= 2 ? gps_num(d, @"latitude") : nil;
	NSNumber *lon = fix >= 2 ? gps_num(d, @"longitude") : nil;
	if (lat && lon) {
		double la = lat.doubleValue, lo = lon.doubleValue;
		((UILabel *)_values[@"Latitude"]).text = [NSString stringWithFormat:@"%.6f° %@",
		    fabs(la), la < 0 ? @"S" : @"N"];
		((UILabel *)_values[@"Longitude"]).text = [NSString stringWithFormat:@"%.6f° %@",
		    fabs(lo), lo < 0 ? @"W" : @"E"];
		_latlonText = [NSString stringWithFormat:@"%.6f, %.6f", la, lo];
	} else {
		((UILabel *)_values[@"Latitude"]).text = @"—";
		((UILabel *)_values[@"Longitude"]).text = @"—";
		_latlonText = nil;
	}
	_copyButton.enabled = _latlonText != nil;
	BOOL pos = fix >= 2;
	((UILabel *)_values[@"Altitude"]).text = pos && fix >= 3 ?
	    gps_fmt(gps_num(d, @"altitude"), @"%.1f m") : @"—";
	NSNumber *ha = gps_num(d, @"hAccuracy"), *va = gps_num(d, @"vAccuracy");
	((UILabel *)_values[@"Accuracy (h / v)"]).text = pos ? [NSString stringWithFormat:@"%@ / %@",
	    gps_fmt(ha, @"±%.1f m"), gps_fmt(va, @"±%.1f m")] : @"—";
	((UILabel *)_values[@"HDOP / VDOP / PDOP"]).text = d ? [NSString stringWithFormat:@"%@ / %@ / %@",
	    gps_fmt(gps_num(d, @"hdop"), @"%.1f"), gps_fmt(gps_num(d, @"vdop"), @"%.1f"),
	    gps_fmt(gps_num(d, @"pdop"), @"%.1f")] : @"—";
	NSNumber *sp = gps_num(d, @"speed");
	((UILabel *)_values[@"Speed"]).text = pos && sp ? [NSString stringWithFormat:@"%.1f m/s  (%.1f km/h)",
	    sp.doubleValue, sp.doubleValue * 3.6] : @"—";
	((UILabel *)_values[@"Course"]).text = pos ? gps_fmt(gps_num(d, @"course"), @"%.0f°") : @"—";
	((UILabel *)_values[@"UTC"]).text = gps_str(d, @"utc") ?: @"—";
}

- (BOOL)prefersStatusBarHidden { return NO; }

@end

/* ------------------------------------------------------------- the application */

@interface GPSDelegate : NSObject <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@property (nonatomic, strong) GPSController *controller;
@end

@implementation GPSDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)options
{
	(void)application;
	self.window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
	self.controller = [[GPSController alloc] init];
	self.window.rootViewController = self.controller;
	[self.window makeKeyAndVisible];
	(void)options;    /* a launch URL comes to -application:openURL:options: by itself */
	return YES;
}

/* hakiosgps://start and hakiosgps://pause press the buttons; anything else just opens */
- (BOOL)application:(UIApplication *)app openURL:(NSURL *)url
    options:(NSDictionary *)options
{
	(void)app; (void)options;
	if ([url.host isEqualToString:@"start"])
		[self.controller sendStart];
	else if ([url.host isEqualToString:@"pause"])
		[self.controller sendPause];
	return YES;
}

- (void)applicationWillResignActive:(UIApplication *)a { (void)a; [self.controller leaveForeground]; }
- (void)applicationDidEnterBackground:(UIApplication *)a { (void)a; [self.controller leaveForeground]; }
- (void)applicationDidBecomeActive:(UIApplication *)a
{
	(void)a;
	[self.controller enterForeground];
}

@end

int
main(int argc, char *argv[])
{
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([GPSDelegate class]));
	}
}
