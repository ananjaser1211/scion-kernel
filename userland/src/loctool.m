// loctool: Location Services from the command line (diagnostics; Scion).
//   loctool status [bundle...]     the global switch, and each bundle's authorization
//   loctool enable | disable       the global switch (Settings > Privacy > Location Services)
//   loctool auth BUNDLE N          a bundle's authorization (setAuthorizationStatusByType)
//   loctool get [SECONDS] [BUNDLE] ask for a position; prints accuracy and source, never the coordinates
//   loctool sim LAT LON [ACC]      a simulated position (CLSimulationManager); loctool sim stop
#import <Foundation/Foundation.h>
#import <CoreLocation/CoreLocation.h>

@interface CLLocationManager (Private)
+ (void)setLocationServicesEnabled:(BOOL)on;
+ (int)authorizationStatusForBundleIdentifier:(NSString *)b;
+ (void)setAuthorizationStatusByType:(int)s forBundleIdentifier:(NSString *)b;
- (id)initWithEffectiveBundleIdentifier:(NSString *)b;
@end
@interface CLSimulationManager : NSObject
@property (assign) uint8_t locationDeliveryBehavior;
@property (assign) uint8_t locationRepeatBehavior;
- (void)clearSimulatedLocations;
- (void)appendSimulatedLocation:(CLLocation *)l;
- (void)flush;
- (void)startLocationSimulation;
- (void)stopLocationSimulation;
@end

@interface Del : NSObject <CLLocationManagerDelegate>
@property int got;
@end
@implementation Del
- (void)locationManager:(CLLocationManager *)m didUpdateLocations:(NSArray *)locs {
	for (CLLocation *l in locs) {
		self.got++;
		NSString *src = @"";
		if ([l respondsToSelector:@selector(sourceInformation)] && l.sourceInformation)
			src = l.sourceInformation.isSimulatedBySoftware ? @" simulated" : @"";
		printf("fix: accuracy %.0f m, altitude acc %.0f, age %.0f s, has-coords %d%s\n",
		       l.horizontalAccuracy, l.verticalAccuracy, -[l.timestamp timeIntervalSinceNow],
		       CLLocationCoordinate2DIsValid(l.coordinate) && (l.coordinate.latitude != 0 || l.coordinate.longitude != 0),
		       src.UTF8String);
		if (getenv("LOCTOOL_SHOW")) printf("  %s\n", l.description.UTF8String);
		fflush(stdout);
	}
}
- (void)locationManager:(CLLocationManager *)m didFailWithError:(NSError *)e {
	printf("error: %s\n", e.description.UTF8String);
	fflush(stdout);
}
- (void)locationManagerDidChangeAuthorization:(CLLocationManager *)m {
	printf("authorization now %d\n", m.authorizationStatus);
	fflush(stdout);
}
@end

int main(int argc, char **argv) {
	@autoreleasepool {
		NSString *cmd = argc > 1 ? @(argv[1]) : @"status";
		if ([cmd isEqual:@"status"]) {
			printf("locationServicesEnabled %d\n", [CLLocationManager locationServicesEnabled]);
			NSArray *b = argc > 2 ? [[NSProcessInfo processInfo].arguments subarrayWithRange:NSMakeRange(2, argc - 2)]
			                      : @[ @"com.apple.Maps", @"com.apple.weather", @"com.apple.mobilecal" ];
			for (NSString *x in b)
				printf("%s %d\n", x.UTF8String, [CLLocationManager authorizationStatusForBundleIdentifier:x]);
		} else if ([cmd isEqual:@"enable"] || [cmd isEqual:@"disable"]) {
			[CLLocationManager setLocationServicesEnabled:[cmd isEqual:@"enable"]];
			[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1]];
			printf("locationServicesEnabled %d\n", [CLLocationManager locationServicesEnabled]);
		} else if ([cmd isEqual:@"auth"] && argc > 3) {
			[CLLocationManager setAuthorizationStatusByType:atoi(argv[3]) forBundleIdentifier:@(argv[2])];
			[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1]];
			printf("%s %d\n", argv[2], [CLLocationManager authorizationStatusForBundleIdentifier:@(argv[2])]);
		} else if ([cmd isEqual:@"get"]) {
			double secs = argc > 2 ? atof(argv[2]) : 30;
			Del *d = [Del new];
			CLLocationManager *m = argc > 3 ? [[CLLocationManager alloc] initWithEffectiveBundleIdentifier:@(argv[3])]
			                                : [CLLocationManager new];
			m.delegate = d;
			m.desiredAccuracy = kCLLocationAccuracyBest;
			printf("authorization %d, enabled %d\n", m.authorizationStatus, [CLLocationManager locationServicesEnabled]);
			fflush(stdout);
			[m startUpdatingLocation];
			NSDate *end = [NSDate dateWithTimeIntervalSinceNow:secs];
			while (!d.got && [end timeIntervalSinceNow] > 0)
				[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];
			[m stopUpdatingLocation];
			if (!d.got) printf("no fix in %.0f s\n", secs);
			CLLocation *last = m.location;
			if (last) printf("manager.location: accuracy %.0f m, age %.0f s\n", last.horizontalAccuracy, -[last.timestamp timeIntervalSinceNow]);
			return d.got ? 0 : 1;
		} else if ([cmd isEqual:@"sim"] && argc > 2) {
			CLSimulationManager *s = [NSClassFromString(@"CLSimulationManager") new];
			if ([@(argv[2]) isEqual:@"stop"]) {
				[s stopLocationSimulation];
				[s clearSimulatedLocations];
				[s flush];
			} else {
				CLLocationCoordinate2D c = CLLocationCoordinate2DMake(atof(argv[2]), atof(argv[3]));
				CLLocation *l = [[CLLocation alloc] initWithCoordinate:c altitude:0 horizontalAccuracy:argc > 4 ? atof(argv[4]) : 10
				                                      verticalAccuracy:-1 timestamp:[NSDate date]];
				[s stopLocationSimulation];
				[s clearSimulatedLocations];
				s.locationDeliveryBehavior = 0;
				s.locationRepeatBehavior = 1;
				[s appendSimulatedLocation:l];
				[s flush];
				[s startLocationSimulation];
			}
			[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1]];
			printf("ok\n");
		} else {
			fprintf(stderr, "usage: loctool status|enable|disable|auth B N|get [S] [B]|sim LAT LON [ACC]|sim stop\n");
			return 2;
		}
	}
	return 0;
}
