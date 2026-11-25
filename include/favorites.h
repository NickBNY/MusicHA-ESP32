#pragma once
/**
 * Sonos Favorites on the player, and the "last played source".
 *
 * The first FAV_SHOWN favorites are fetched in the background (one Browse of FV:2)
 * and kept in memory, so the player can draw them without a network call and a
 * tap starts playback without ever blocking the UI thread.
 *
 * The last source a user started - from a favorite here or from the Sources
 * screen - is written to NVS so that Play on a speaker with nothing queued can
 * start it again.
 */
#include <Arduino.h>

int         favoritesCount();             // 0..FAV_SHOWN
const char* favoritesTitle(int index);    // "" when out of range; not locked
// Copies a title under the list lock (5 ms wait). False on timeout or out of range.
bool        favoritesTitleCopy(int index, char* out, size_t n);
uint32_t    favoritesVersion();           // changes whenever the list does
void        favoritesRequestRefresh();    // fetch at the next idle moment
void        favoritesTick();              // from mainAppTask; cheap
void        favoritesPlay(int index);     // starts playback off the UI thread

// Plays a favorite / browse item given its DIDL <item> XML, exactly as the Sources
// screen does, and remembers it as the last source. Blocking: call off the UI thread.
bool        favoritePlayItemXML(const String& itemXML);

// The last source. Save is cheap and must run on mainAppTask (NVS write).
bool        lastSourceExists();
String      lastSourceTitle();
void        lastSourceSave(char kind, const String& uri, const String& meta, const String& title);
void        lastSourcePlay();             // async; no-op when nothing was saved

// Opens the Favorites browser (the player's album art does this when tapped).
void        openFavoritesScreen();
