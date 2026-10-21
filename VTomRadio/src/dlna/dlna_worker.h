#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

enum DlnaJobType : uint8_t { DJ_INIT=0, DJ_BUILD=1, DJ_APPEND=2, DJ_CANCEL=3, DJ_LIST=4 };

struct DlnaJob {
  DlnaJobType type;
  char objectId[64];   // DLNA objectId
//  char key[32];        // opcionális: category/genre key
  uint32_t reqId;      // monoton növekvő request id
  uint32_t hardLimit;
  uint32_t start;      // DJ_LIST: lapozási kezdőindex
};

struct DlnaStatus {
  volatile bool busy;
  volatile bool ok;
  volatile int  err;           // 0=OK, HTTP-szerű / belső hibakód
  volatile uint32_t reqId;
  volatile uint32_t playlistVer;   // fájlverzió a preview sync-hez
  char msg[96];
};

extern QueueHandle_t g_dlnaQueue;
extern SemaphoreHandle_t g_littlefsMux;
extern DlnaStatus g_dlnaStatus;

void dlna_worker_start();
bool dlna_worker_enqueue(const DlnaJob& j);

// Aszinkron lista: a böngészés a worker taskban fut, a web szerver szál nem blokkol
bool dlna_list_submit(const char* objectId, uint32_t start, uint32_t reqId);
// 0 = folyamatban, 1 = kész (out kitöltve), -1 = hiba, -2 = ismeretlen reqId
int  dlna_list_result(uint32_t reqId, String& out);
void dlna_status_setBusy(const DlnaJob& j, const char* msg);
void dlna_status_setDone(const DlnaJob& j, bool ok, int err, const char* msg);
uint32_t dlna_playlist_version();
