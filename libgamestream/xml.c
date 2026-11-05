/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "xml.h"
#include "errors.h"

#include <expat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

#define STATUS_OK 200

struct xml_query {
  char *memory;
  size_t size;
  int start;
  void* data;
};

static void XMLCALL _xml_start_element(void *userData, const char *name, const char **atts) {
  struct xml_query *search = (struct xml_query*) userData;
  if (strcmp(search->data, name) == 0)
    search->start++;
}

static void XMLCALL _xml_end_element(void *userData, const char *name) {
  struct xml_query *search = (struct xml_query*) userData;
  if (strcmp(search->data, name) == 0)
    search->start--;
}

static void XMLCALL _xml_start_applist_element(void *userData, const char *name, const char **atts) {
  struct xml_query *search = (struct xml_query*) userData;
  if (strcmp("App", name) == 0) {
    PAPP_LIST app = malloc(sizeof(APP_LIST));
    if (app == NULL)
      return;

    app->id = 0;
    app->name = NULL;
    app->next = (PAPP_LIST) search->data;
    search->data = app;
  } else if (strcmp("ID", name) == 0 || strcmp("AppTitle", name) == 0) {
    search->memory = malloc(1);
    search->size = 0;
    search->start = 1;
  }
}

static void XMLCALL _xml_end_applist_element(void *userData, const char *name) {
  struct xml_query *search = (struct xml_query*) userData;
  if (search->start) {
    PAPP_LIST list = (PAPP_LIST) search->data;
    if (list == NULL)
      return;

    if (strcmp("ID", name) == 0) {
        list->id = atoi(search->memory);
        free(search->memory);
    } else if (strcmp("AppTitle", name) == 0) {
        list->name = search->memory;
    }
    search->start = 0;
  }
}

static void XMLCALL _xml_start_mode_element(void *userData, const char *name, const char **atts) {
  struct xml_query *search = (struct xml_query*) userData;
  if (strcmp("DisplayMode", name) == 0) {
    PDISPLAY_MODE mode = calloc(1, sizeof(DISPLAY_MODE));
    if (mode != NULL) {
      mode->next = (PDISPLAY_MODE) search->data;
      search->data = mode;
    }
  } else if (search->data != NULL && (strcmp("Height", name) == 0 || strcmp("Width", name) == 0 || strcmp("RefreshRate", name) == 0)) {
    search->memory = malloc(1);
    search->size = 0;
    search->start = 1;
  }
}

static void XMLCALL _xml_end_mode_element(void *userData, const char *name) {
  struct xml_query *search = (struct xml_query*) userData;
  if (search->data != NULL && search->start) {
    PDISPLAY_MODE mode = (PDISPLAY_MODE) search->data;
    if (strcmp("Width", name) == 0)
      mode->width = atoi(search->memory);
    else if (strcmp("Height", name) == 0)
      mode->height = atoi(search->memory);
    else if (strcmp("RefreshRate", name) == 0)
      mode->refresh = atoi(search->memory);

    free(search->memory);
    search->start = 0;
  }
}

struct xml_status_info {
  int status_code;
  int has_status_code;
  char *status_message;
  size_t status_message_size;
};

static void XMLCALL _xml_start_status_element(void *userData, const char *name, const char **atts) {
  if (strcmp("root", name) == 0) {
    struct xml_status_info *status = (struct xml_status_info*) userData;
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp("status_code", atts[i]) == 0) {
        status->status_code = atoi(atts[i + 1]);
        status->has_status_code = 1;
      } else if (strcmp("status_message", atts[i]) == 0 &&
                 status->status_message != NULL &&
                 status->status_message_size > 0) {
        snprintf(status->status_message, status->status_message_size, "%s",
                 atts[i + 1]);
      }
    }
  }
}

static void XMLCALL _xml_end_status_element(void *userData, const char *name) { }

static void XMLCALL _xml_write_data(void *userData, const XML_Char *s, int len) {
  struct xml_query *search = (struct xml_query*) userData;
  if (search->start > 0) {
    search->memory = realloc(search->memory, search->size + len + 1);
    if(search->memory == NULL)
      return;

    memcpy(&(search->memory[search->size]), s, len);
    search->size += len;
    search->memory[search->size] = 0;
  }
}

enum display_field { DISPLAY_FIELD_NONE, DISPLAY_FIELD_ID, DISPLAY_FIELD_NAME };

struct display_query {
  XML_Parser parser;
  PDISPLAY_INFO head;
  PDISPLAY_INFO tail;
  PDISPLAY_INFO current;
  char *value;
  size_t value_size;
  enum display_field field;
  int failed;
};

static void free_display_nodes(PDISPLAY_INFO display) {
  while (display != NULL) {
    PDISPLAY_INFO next = display->next;
    free(display->id);
    free(display->name);
    free(display);
    display = next;
  }
}

static void XMLCALL _xml_start_display_element(void *userData, const char *name,
                                                const char **atts) {
  struct display_query *query = (struct display_query*) userData;
  if (strcmp("display", name) == 0) {
    PDISPLAY_INFO display = (PDISPLAY_INFO) calloc(1, sizeof(DISPLAY_INFO));
    if (display == NULL) {
      query->failed = 1;
      XML_StopParser(query->parser, XML_FALSE);
      return;
    }
    if (query->tail == NULL)
      query->head = display;
    else
      query->tail->next = display;
    query->tail = display;
    query->current = display;
  } else if (query->current != NULL &&
             (strcmp("id", name) == 0 || strcmp("name", name) == 0)) {
    free(query->value);
    query->value = (char*) calloc(1, 1);
    if (query->value == NULL) {
      query->failed = 1;
      XML_StopParser(query->parser, XML_FALSE);
      return;
    }
    query->value_size = 0;
    query->field = strcmp("id", name) == 0 ? DISPLAY_FIELD_ID
                                             : DISPLAY_FIELD_NAME;
  }
}

static void XMLCALL _xml_end_display_element(void *userData, const char *name) {
  struct display_query *query = (struct display_query*) userData;
  if (query->field != DISPLAY_FIELD_NONE && query->value != NULL &&
      ((query->field == DISPLAY_FIELD_ID && strcmp("id", name) == 0) ||
       (query->field == DISPLAY_FIELD_NAME && strcmp("name", name) == 0))) {
    char *start = query->value;
    while (*start != '\0' && isspace((unsigned char) *start)) ++start;
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char) end[-1])) --end;
    *end = '\0';
    if (start != query->value)
      memmove(query->value, start, (size_t) (end - start) + 1);
    if (query->field == DISPLAY_FIELD_ID) {
      free(query->current->id);
      query->current->id = query->value;
    } else {
      free(query->current->name);
      query->current->name = query->value;
    }
    query->value = NULL;
    query->value_size = 0;
    query->field = DISPLAY_FIELD_NONE;
  } else if (strcmp("display", name) == 0) {
    query->current = NULL;
  }
}

static void XMLCALL _xml_write_display_data(void *userData, const XML_Char *s,
                                             int len) {
  struct display_query *query = (struct display_query*) userData;
  if (query->field == DISPLAY_FIELD_NONE || query->failed || len <= 0)
    return;
  char *expanded = (char*) realloc(query->value, query->value_size + (size_t) len + 1);
  if (expanded == NULL) {
    query->failed = 1;
    XML_StopParser(query->parser, XML_FALSE);
    return;
  }
  query->value = expanded;
  memcpy(query->value + query->value_size, s, (size_t) len);
  query->value_size += (size_t) len;
  query->value[query->value_size] = '\0';
}

int xml_display_list(char* data, size_t len, PDISPLAY_INFO *display_list) {
  struct display_query query = {0};
  if (display_list == NULL || data == NULL || len > INT_MAX)
    return GS_INVALID;
  *display_list = NULL;

  query.parser = XML_ParserCreate("UTF-8");
  if (query.parser == NULL)
    return GS_OUT_OF_MEMORY;
  XML_SetUserData(query.parser, &query);
  XML_SetElementHandler(query.parser, _xml_start_display_element,
                        _xml_end_display_element);
  XML_SetCharacterDataHandler(query.parser, _xml_write_display_data);
  const int parsed = XML_Parse(query.parser, data, (int) len, XML_TRUE);
  if (!parsed && !query.failed)
    gs_error = XML_ErrorString(XML_GetErrorCode(query.parser));
  XML_ParserFree(query.parser);
  free(query.value);

  if (query.failed || !parsed) {
    free_display_nodes(query.head);
    return query.failed ? GS_OUT_OF_MEMORY : GS_INVALID;
  }

  // Ignore malformed entries without a stable ID. The UI uses the ID as a
  // readable fallback when a host omits a display name.
  PDISPLAY_INFO *link = &query.head;
  query.tail = NULL;
  while (*link != NULL) {
    PDISPLAY_INFO current = *link;
    if (current->id == NULL || current->id[0] == '\0') {
      *link = current->next;
      current->next = NULL;
      free_display_nodes(current);
      continue;
    }
    query.tail = current;
    link = &current->next;
  }
  *display_list = query.head;
  return GS_OK;
}

int xml_search(char* data, size_t len, char* node, char** result) {
  struct xml_query search;
  search.data = node;
  search.start = 0;
  search.memory = calloc(1, 1);
  search.size = 0;
  XML_Parser parser = XML_ParserCreate("UTF-8");
  XML_SetUserData(parser, &search);
  XML_SetElementHandler(parser, _xml_start_element, _xml_end_element);
  XML_SetCharacterDataHandler(parser, _xml_write_data);
  if (! XML_Parse(parser, data, len, 1)) {
    int code = XML_GetErrorCode(parser);
    gs_error = XML_ErrorString(code);
    XML_ParserFree(parser);
    free(search.memory);
    return GS_INVALID;
  } else if (search.memory == NULL) {
    XML_ParserFree(parser);
    return GS_OUT_OF_MEMORY;
  }

  XML_ParserFree(parser);
  *result = search.memory;

  return GS_OK;
}

int xml_applist(char* data, size_t len, PAPP_LIST *app_list) {
  struct xml_query query;
  query.memory = calloc(1, 1);
  query.size = 0;
  query.start = 0;
  query.data = NULL;
  XML_Parser parser = XML_ParserCreate("UTF-8");
  XML_SetUserData(parser, &query);
  XML_SetElementHandler(parser, _xml_start_applist_element, _xml_end_applist_element);
  XML_SetCharacterDataHandler(parser, _xml_write_data);
  if (! XML_Parse(parser, data, len, 1)) {
    int code = XML_GetErrorCode(parser);
    gs_error = XML_ErrorString(code);
    XML_ParserFree(parser);
    return GS_INVALID;
  }

  XML_ParserFree(parser);
  *app_list = (PAPP_LIST) query.data;

  return GS_OK;
}

int xml_modelist(char* data, size_t len, PDISPLAY_MODE *mode_list) {
  struct xml_query query = {0};
  query.memory = calloc(1, 1);
  XML_Parser parser = XML_ParserCreate("UTF-8");
  XML_SetUserData(parser, &query);
  XML_SetElementHandler(parser, _xml_start_mode_element, _xml_end_mode_element);
  XML_SetCharacterDataHandler(parser, _xml_write_data);
  if (! XML_Parse(parser, data, len, 1)) {
    int code = XML_GetErrorCode(parser);
    gs_error = XML_ErrorString(code);
    XML_ParserFree(parser);
    return GS_INVALID;
  }

  XML_ParserFree(parser);
  *mode_list = (PDISPLAY_MODE) query.data;

  return GS_OK;

}

int xml_status_details(char* data, size_t len, int *status_code,
                       int *has_status_code, char *status_message,
                       size_t status_message_size) {
  static char status_error[160];
  struct xml_status_info status = {0};
  if (status_code != NULL)
    *status_code = 0;
  if (has_status_code != NULL)
    *has_status_code = 0;
  if (status_message != NULL && status_message_size > 0)
    status_message[0] = '\0';
  status.status_message = status_message;
  status.status_message_size = status_message_size;
  XML_Parser parser = XML_ParserCreate("UTF-8");
  if (parser == NULL)
    return GS_OUT_OF_MEMORY;
  XML_SetUserData(parser, &status);
  XML_SetElementHandler(parser, _xml_start_status_element, _xml_end_status_element);
  if (!XML_Parse(parser, data, len, 1)) {
    int code = XML_GetErrorCode(parser);
    gs_error = XML_ErrorString(code);
    XML_ParserFree(parser);
    return GS_INVALID;
  }

  XML_ParserFree(parser);
  if (status_code != NULL)
    *status_code = status.status_code;
  if (has_status_code != NULL)
    *has_status_code = status.has_status_code;
  if (status.status_code != STATUS_OK && status.has_status_code) {
    snprintf(status_error, sizeof(status_error),
             "GameStream status_code=%d message=%s", status.status_code,
             status.status_message != NULL && status.status_message[0]
                 ? status.status_message : "(missing)");
    gs_error = status_error;
  }
  return status.status_code == STATUS_OK ? GS_OK : GS_ERROR;
}

int xml_status(char* data, size_t len) {
  return xml_status_details(data, len, NULL, NULL, NULL, 0);
}
