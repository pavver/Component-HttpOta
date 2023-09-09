#pragma once

#include "esp_app_format.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

static esp_err_t update_ota(httpd_req_t *req)
{
  const static char *TAG = "OTA";
  esp_err_t err;
  esp_err_t ret = ESP_OK;
  /* update handle : set by esp_ota_begin(), must be freed via esp_ota_end() */
  esp_ota_handle_t update_handle = 0;
  const esp_partition_t *update_partition = NULL;

  const esp_partition_t *configured = esp_ota_get_boot_partition();
  const esp_partition_t *running = esp_ota_get_running_partition();

  if (configured != running)
  {
    ESP_LOGW(TAG, "(This can happen if either the OTA boot data or preferred boot image become corrupted somehow.)");
  }
  ESP_LOGI(TAG, "Running partition type %d", running->type);

  int buf_len = 1024;
  char *buf = (char *)calloc(1, buf_len + 1);

  update_partition = esp_ota_get_next_update_partition(NULL);
  assert(update_partition != NULL);

  int binary_file_length = 0;
  /*deal with all receive packet*/
  bool image_header_was_checked = false;
  while (true)
  {
    int data_read = httpd_req_recv(req, buf, buf_len);
    if (data_read < 0)
    {
      if (data_read == HTTPD_SOCK_ERR_TIMEOUT)
        // Retry if timeout occurred
        continue;

      ret = ESP_FAIL;
      break;
    }
    else if (data_read > 0)
    {
      if (image_header_was_checked == false)
      {
        esp_app_desc_t new_app_info;
        if (data_read > sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))
        {
          // check current version with downloading
          memcpy(&new_app_info, &buf[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)], sizeof(esp_app_desc_t));
          ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);

          esp_app_desc_t running_app_info;
          if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK)
          {
            ESP_LOGI(TAG, "Running firmware version: %s", running_app_info.version);
          }

          const esp_partition_t *last_invalid_app = esp_ota_get_last_invalid_partition();
          esp_app_desc_t invalid_app_info;
          if (esp_ota_get_partition_description(last_invalid_app, &invalid_app_info) == ESP_OK)
          {
            ESP_LOGI(TAG, "Last invalid firmware version: %s", invalid_app_info.version);
          }

          // check current version with last invalid partition
          if (last_invalid_app != NULL)
          {
            if (memcmp(invalid_app_info.version, new_app_info.version, sizeof(new_app_info.version)) == 0)
            {
              ESP_LOGW(TAG, "New version is the same as invalid version.");
              ESP_LOGW(TAG, "Previously, there was an attempt to launch the firmware with %s version, but it failed.", invalid_app_info.version);
              ESP_LOGW(TAG, "The firmware has been rolled back to the previous version.");
              ret = ESP_FAIL;
              break;
            }
          }
          /*  if (memcmp(new_app_info.version, running_app_info.version, sizeof(new_app_info.version)) == 0)
           {
             ESP_LOGW(TAG, "Current running version is the same as a new. We will not continue the update.");
             ret = ESP_FAIL;
             break;
           } */

          image_header_was_checked = true;

          err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
          if (err != ESP_OK)
          {
            ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
            esp_ota_abort(update_handle);
            ret = ESP_FAIL;
            break;
          }
          ESP_LOGI(TAG, "esp_ota_begin succeeded");
        }
        else
        {
          ESP_LOGE(TAG, "received package is not fit len");
          esp_ota_abort(update_handle);
          ret = ESP_FAIL;
          break;
        }
      }
      err = esp_ota_write(update_handle, (const void *)buf, data_read);
      if (err != ESP_OK)
      {
        esp_ota_abort(update_handle);
        ret = ESP_FAIL;
        break;
      }
      binary_file_length += data_read;
      ESP_LOGD(TAG, "Written image length %d", binary_file_length);
    }
    else if (data_read == 0)
    {
      break;
    }
  }

  if (ret == ESP_FAIL)
    return ret;

  ESP_LOGI(TAG, "Total Write binary data length: %d", binary_file_length);

  err = esp_ota_end(update_handle);
  if (err != ESP_OK)
  {
    if (err == ESP_ERR_OTA_VALIDATE_FAILED)
    {
      ESP_LOGE(TAG, "Image validation failed, image is corrupted");
    }
    else
    {
      ESP_LOGE(TAG, "esp_ota_end failed (%s)!", esp_err_to_name(err));
    }
  }

  err = esp_ota_set_boot_partition(update_partition);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
    httpd_resp_send_500(req);
  }
  ESP_LOGI(TAG, "Prepare to restart system!");
  httpd_resp_sendstr_chunk(req, "OK");
  httpd_resp_sendstr_chunk(req, NULL);
  httpd_resp_set_hdr(req, "Connection", "close");

  vTaskDelay(1000 / portTICK_PERIOD_MS);

  esp_restart();
  return ret;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

static const httpd_uri_t http_server_post_ota_request = {
    .uri = "/ota",
    .method = HTTP_POST,
    .handler = update_ota};

#pragma GCC diagnostic pop

static esp_err_t register_ota_handler(httpd_handle_t handle)
{
  return httpd_register_uri_handler(handle, &http_server_post_ota_request);
}