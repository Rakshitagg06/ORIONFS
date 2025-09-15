all:
	@echo "Building all components..."
	@cd naming_server && $(MAKE)
	@cd storage_server && $(MAKE)
	@cd client && $(MAKE)
	@echo "✅ All components built successfully!"

clean:
	@echo "Cleaning all components..."
	@cd naming_server && $(MAKE) clean
	@cd storage_server && $(MAKE) clean
	@cd client && $(MAKE) clean
	@echo "✅ Clean complete!"

nm:
	@cd naming_server && $(MAKE)

ss:
	@cd storage_server && $(MAKE)

client:
	@cd client && $(MAKE)

.PHONY: all clean nm ss client
