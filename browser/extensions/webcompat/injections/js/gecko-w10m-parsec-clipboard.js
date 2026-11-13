/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

(() => {
  if (location.origin !== "https://web.parsec.app") {
    return;
  }

  const clipboard = navigator.clipboard;
  if (typeof clipboard?.readText !== "function") {
    return;
  }

  const originalReadText = clipboard.readText;
  let reportedDenial = false;
  Object.defineProperty(clipboard, "readText", {
    configurable: true,
    enumerable: true,
    writable: true,
    value: async function readText(...args) {
      try {
        return await Reflect.apply(originalReadText, this, args);
      } catch (error) {
        if (this !== clipboard || error?.name !== "NotAllowedError") {
          throw error;
        }
        // Matoya does not wake its waiting worker when readText rejects.
        if (!reportedDenial) {
          reportedDenial = true;
          console.info(
            "gecko-w10m: Parsec clipboard read denied; returning empty text to avoid a blocked worker"
          );
        }
        return "";
      }
    },
  });
})();
