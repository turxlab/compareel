// AppKit adds its own "Enter Full Screen" item to the menu bar when the app finishes launching; Compareel lists
// full screen itself (View, in every interface language), so the automatic duplicate is switched off.
#import <Foundation/Foundation.h>

void DisableAutomaticFullScreenMenuItem() {
    [[NSUserDefaults standardUserDefaults] registerDefaults:@{@"NSFullScreenMenuItemEverywhere": @NO}];
}
