SDK := $(shell xcrun --show-sdk-path)
CFLAGS := -Wall -Wextra -Werror -arch arm64 -isysroot $(SDK) -mmacosx-version-min=15.0 -fPIC
BUILD := build/NumarkNS6.driver/Contents
APP := build/NumarkNS6Status.app
VERSION ?= 0.1.0
PACKAGE_ID := io.github.maurojuniorr.numark-ns6
PACKAGE_ROOT := build/package-root
DIST := dist
all: $(BUILD)/MacOS/NumarkNS6 $(BUILD)/Info.plist app
$(BUILD)/MacOS/NumarkNS6: NS6HAL.c NS6Transport.c NS6USB.c
	mkdir -p $(BUILD)/MacOS
	$(CC) $(CFLAGS) -bundle NS6HAL.c NS6Transport.c NS6USB.c -o $@ -framework CoreAudio -framework CoreFoundation -framework IOKit
$(BUILD)/Info.plist: Info.plist
	mkdir -p $(BUILD)
	cp $< $@
app: $(APP)/Contents/MacOS/NumarkNS6Status $(APP)/Contents/Info.plist $(APP)/Contents/Resources/NS6Status.icns
$(APP)/Contents/MacOS/NumarkNS6Status: status-app/NS6Status.swift
	mkdir -p $(APP)/Contents/MacOS
	swiftc -O -target arm64-apple-macosx15.0 $< -o $@ -framework AppKit -framework CoreAudio -framework IOKit
$(APP)/Contents/Info.plist: status-app/Info.plist
	mkdir -p $(APP)/Contents
	cp $< $@
$(APP)/Contents/Resources/NS6Status.icns: status-app/NS6Status.icns
	mkdir -p $(APP)/Contents/Resources
	cp $< $@
clean:
	rm -rf build

usb-test: NS6USBTest.c NS6Transport.c NS6USB.c
	$(CC) $(CFLAGS) NS6USBTest.c NS6Transport.c NS6USB.c -o build/ns6-usb-test -framework CoreAudio -framework CoreFoundation -framework IOKit -lm

audio-list: AudioList.c
	$(CC) $(CFLAGS) AudioList.c -o build/audio-list -framework CoreAudio -framework CoreFoundation

audio-default: AudioDefault.c
	$(CC) $(CFLAGS) AudioDefault.c -o build/audio-default -framework CoreAudio -framework CoreFoundation

sign: all
	codesign --force --sign - --timestamp=none build/NumarkNS6.driver
	codesign --verify --strict --verbose=2 build/NumarkNS6.driver
	codesign --force --sign - --timestamp=none $(APP)
	codesign --verify --strict --verbose=2 $(APP)

install: sign
	sudo ditto build/NumarkNS6.driver /Library/Audio/Plug-Ins/HAL/NumarkNS6M1.driver
	sudo ditto $(APP) '/Applications/Numark NS6 Status.app'
	sudo killall coreaudiod

package: sign
	rm -rf build/package-root dist
	mkdir -p build/package-root/Library/Audio/Plug-Ins/HAL build/package-root/Applications dist
	ditto --norsrc --noextattr build/NumarkNS6.driver build/package-root/Library/Audio/Plug-Ins/HAL/NumarkNS6M1.driver
	ditto --norsrc --noextattr $(APP) 'build/package-root/Applications/Numark NS6 Status.app'
	xattr -cr build/package-root
	pkgbuild --root build/package-root --scripts package-scripts --identifier $(PACKAGE_ID) --version $(VERSION) --install-location / dist/NumarkNS6-$(VERSION).pkg

uninstall:
	sudo rm -rf /Library/Audio/Plug-Ins/HAL/NumarkNS6M1.driver
	sudo killall coreaudiod
