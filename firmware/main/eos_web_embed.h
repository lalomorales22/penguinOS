// Binds eos_httpd's three file ports to the web app linked into the image.
// Call it AFTER eos_httpd_idf_bind(), which deliberately leaves them NULL.
#ifndef EOS_WEB_EMBED_H
#define EOS_WEB_EMBED_H
#include "eos_httpd.h"
void eos_web_embed_bind(eos_httpd_t *h);
#endif
