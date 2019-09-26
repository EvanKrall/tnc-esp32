
$(COMPONENT_PATH)/cert.pem: $(COMPONENT_PATH)/private.key
$(COMPONENT_PATH)/private.key:
	openssl req -newkey rsa:2048 -nodes -keyout $(COMPONENT_PATH)/private.key -x509 -days 3650 -out $(COMPONENT_PATH)/cert.pem -subj "/CN=$(PROJECT_NAME)"

COMPONENT_EMBED_TXTFILES += www_data/index.html
COMPONENT_EMBED_TXTFILES += cert.pem private.key