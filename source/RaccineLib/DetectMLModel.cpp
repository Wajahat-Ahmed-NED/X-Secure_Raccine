#include <iostream>
#include <curl/curl.h>

size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

int main(data) {
    CURL* curl;
    CURLcode res;
    std::string api_url = "https://runtime.sagemaker.us-east-1.amazonaws.com/endpoints/sagemaker-xgboost-2023-07-30-12-01-50-187/invocations";  // Replace with your SageMaker endpoint URL
    std::string json_payload = data;  
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();

    if (curl) {
        // Set the API URL
        curl_easy_setopt(curl, CURLOPT_URL, api_url.c_str());

        // Set the POST data
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload.c_str());

        // Set the callback function to handle response
        std::string response_data;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);

        // Perform the request
        res = curl_easy_perform(curl);

        // Check for errors
        if (res != CURLE_OK) {
            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
        } else {
            return response_data;
        }

        // Clean up
        curl_easy_cleanup(curl);
    }

    curl_global_cleanup();
    return 0;
}
