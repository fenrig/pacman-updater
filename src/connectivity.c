#include "connectivity.h"
#include <curl/curl.h>

static int connectivity_probe(const char *endpoint, int64_t timeout)
{
	CURL *curl = curl_easy_init();
	long response_code = 0;
	CURLcode result;

	if (!curl)
		return 0;
	curl_easy_setopt(curl, CURLOPT_URL, endpoint);
	curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)timeout);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
	result = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
	curl_easy_cleanup(curl);
	return result == CURLE_OK && response_code == 204;
}

int connectivity_check(const config_t *config)
{
	if (connectivity_probe(config->endpoint, config->timeout))
		return 1;
	return config->fallback_endpoint[0]
	       && connectivity_probe(config->fallback_endpoint,
				     config->timeout);
}
