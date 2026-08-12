/*
 * Browsing and editing the SD card over HTTP.
 *
 * This is the other half of moving everything onto the card: being able to see
 * what is there and change it without prying the card out of the board. List a
 * directory, read a file, write one, rename, delete, make a folder, and unpack
 * a ZIP - all from the panel or from curl.
 *
 * Every route here demands the token. That is not optional even for reading: a
 * file API that will fetch any path would happily hand out
 * /Concordia/security/api_token to anyone who asked.
 *
 * Every path is confined to /Concordia. There is nothing else on the card that
 * belongs to this firmware, and a file manager that can wander into the rest of
 * someone's card is a liability, not a feature.
 */
#pragma once

#include <WebServer.h>

namespace FsApi {

void attach(WebServer &server);

} // namespace FsApi
