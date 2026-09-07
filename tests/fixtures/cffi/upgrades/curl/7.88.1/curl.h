#ifndef CHTHOLLY_CURL_7881_H
#define CHTHOLLY_CURL_7881_H
#define LIBCURL_VERSION_NUM 0x075801
typedef struct CURL CURL;
typedef int CURLcode;
CURL *curl_easy_init(void);
void curl_easy_cleanup(CURL *handle);
const char *curl_easy_strerror(CURLcode error);
CURLcode curl_easy_perform(CURL *handle);
#endif
