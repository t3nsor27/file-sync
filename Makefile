CXX = g++

.PHONY = run, build
build: $(FILE).cpp
	@echo "Building $(FILE).cpp"
	@$(CXX) -std=c++20 $(FILE).cpp -I$(ICPP) -I./include/ -L$(LCPP) \
	./src/fstree.cpp \
	./src/peer.cpp \
	./src/wire.cpp \
  -lftxui-component -lftxui-dom -lftxui-screen \
  -pthread -ldl -lcrypto -o ./misc/build/$(notdir $(FILE))
	@echo "Done"

run: $(FILE).cpp
	@echo "Building $(FILE).cpp"
	@$(CXX) -std=c++20 $(FILE).cpp -I$(ICPP) -I./include/ -L$(LCPP) \
	./src/fstree.cpp \
	./src/peer.cpp \
	./src/wire.cpp \
  -lftxui-component -lftxui-dom -lftxui-screen \
  -pthread -ldl -lcrypto -o ./misc/build/$(notdir $(FILE))
	@clear
	@./misc/build/$(FILE)

zip:
	@zip -r ../file-sync.zip . -r --exclude "./.git/*" --exclude "./misc/*"
