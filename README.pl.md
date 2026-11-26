**# Termostat pokojowy Wi-Fi ESP32 — Master + Slave**



Bezprzewodowy termostat pokojowy do sterowania kotłem lub innym urządzeniem grzewczym wyposażonym w beznapięciowe wejście sterujące.



System składa się z dwóch modułów komunikujących się przez lokalną sieć Wi-Fi. \*\*Nie wymaga chmury ani dostępu do internetu.\*\*



**## Podzespoły**



\*\*Master — sterownik pokojowy\*\*

**-&#x20;**&#x4E;iceMCU-32S-DEV 2.8IPS z mikrokontrolerem ESP32.

**-&#x20;**&#x4B;olorowy wyświetlacz IPS ST7789 320 × 240.

**-&#x20;**&#x50;ojemnościowy panel dotykowy CST816D.

**-&#x20;**&#x43;zujnik BME280 mierzący temperaturę, wilgotność i ciśnienie atmosferyczne.



\*\*Slave — moduł wykonawczy\*\*

**-&#x20;**&#x45;SP32-C3 SuperMini.

**-&#x20;**&#x4D;oduł przekaźnika 5 V z wejściem aktywnym stanem LOW.

**-&#x20;**&#x54;ranzystor NPN BC337 sterujący wejściem przekaźnika.

**-&#x20;**&#x5A;asilacz 230 V AC → 5 V DC.



**## Jak to działa?**



Master mierzy temperaturę pomieszczenia i porównuje ją z temperaturą zadaną oraz ustawioną histerezą. Na tej podstawie wysyła do Slave polecenie włączenia lub wyłączenia przekaźnika sterującego kotłem.



Na ekranie dotykowym można zmieniać temperaturę zadaną i histerezę oraz sprawdzać temperaturę, wilgotność, ciśnienie atmosferyczne, stan komunikacji TCP i siłę sygnału Wi-Fi obu urządzeń.



Moduły komunikują się za pomocą \*\*szyfrowanego połączenia TCP w lokalnej sieci Wi-Fi\*\*. Pierwsza konfiguracja odbywa się przez Bluetooth Low Energy (BLE), który służy do uwierzytelnienia Slave i bezpiecznego przekazania danych sieciowych.



Slave podczas nawiązywania połączenia Wi-Fi wyszukuje punkt dostępowy o najsilniejszym sygnale dla skonfigurowanej sieci, co ułatwia pracę w środowisku Wi-Fi Mesh.



**### Zabezpieczenie na wypadek utraty łączności**



Krótkie przerwy w komunikacji nie powodują natychmiastowego wyłączenia ogrzewania.



Po utracie poprawnych poleceń Slave zachowuje dotychczasowy stan przekaźnika do upływu skonfigurowanego czasu awaryjnego. Następnie wykonuje wybraną akcję:



**-&#x20;**\*\*OFF\*\* — wyłączenie przekaźnika.

**-&#x20;**\*\*ON\*\* — włączenie przekaźnika.

**-&#x20;**\*\*OSTATNI STAN\*\* — pozostawienie przekaźnika bez zmian.



Czas reakcji można ustawić od \*\*1 do 30 minut\*\* (domyślnie 10 minut). Ustawienia są przekazywane z Mastera do Slave i zapisywane w pamięci urządzenia.



Po uruchomieniu Slave bez odebranego polecenia stan przekaźnika jest początkowo OFF.



**## Integracja z Apple HomeKit i Home Assistant**



Master obsługuje Apple HomeKit, dzięki czemu można odczytywać temperaturę i zmieniać temperaturę zadaną z poziomu iPhone'a lub Home Assistant, wykorzystując integrację HomeKit Device.



Podstawowe sterowanie kotłem pozostaje lokalne i nie zależy od dostępności internetu ani usług zewnętrznych.



**## Klucz szyfrowania AES-256**



\*\*Przed wgraniem oprogramowania należy wygenerować własny, losowy klucz AES-256.\*\*



Klucz ma długość 32 bajtów i należy go umieścić w pliku \`SecureLink.h\`:



\`\`\`cpp

**static** **const** uint8_t KEY[32] = {

    *// Tutaj 32 bajty wygenerowanego klucza*

};

\`\`\`



Ten sam klucz musi być zastosowany w firmware \*\*Mastera i Slave\*\*. W przeciwnym razie uwierzytelnianie i szyfrowana komunikacja nie będą działały.



Generowanie klucza w PowerShell:



\`\`\`powershell

$key = New-Object byte[] 32

[System.Security.Cryptography.RandomNumberGenerator]\::Fill($key)

($key | ForEach-Object { "0x{0:x2}" -f $\_ }) -join ", "

\`\`\`



Generowanie klucza w Bash:



\`\`\`bash

openssl rand -hex 32 | sed 's/../0x&, /g; s/, $//'

\`\`\`



Klucza nie należy publikować w publicznym repozytorium Git.



**## Dlaczego NiceMCU zamiast ESP32-CYD?**



Pierwowzór termostatu działał na popularnej płytce \*\*ESP32-CYD (Cheap Yellow Display)\*\* z ekranem dotykowym 320 × 240 i wbudowanym czujnikiem światła. Ten ostatni w chińskich płytkach działa różnie. :)



Ze względu na ograniczoną ilość dostępnej pamięci RAM — program działał już praktycznie „na styk” — zdecydowałem się na zmianę płytki na \*\*NiceMCU-32S-DEV 2.8IPS\*\*.



Cena jest zbliżona, a nowa płytka oferuje nieco lepszy wyświetlacz IPS oraz pojemnościowy panel dotykowy CST816D, który obsługuję bez dodatkowej biblioteki dotyku. Pozwoliło to ograniczyć wykorzystanie pamięci RAM.



![Płytka NiceMCU Master]\(images/plyta-nowa.jpg){width=400}



Czujnik BME280 jest podłączony do złącza oznaczonego jako I2C.



![Złącze I2C Mastera]\(images/master-i2c.jpg){width=400}



**## Moduł wykonawczy Slave**



W części wykonawczej, zamontowanej przy kotle, zastosowałem:



**-&#x20;**&#x45;SP32-C3 SuperMini.

**-&#x20;**&#x4D;oduł przekaźnika 5 V wyzwalany stanem niskim.

**-&#x20;**&#x54;ranzystor BC337.

**-&#x20;**&#x52;ezystor 10 kΩ.

**-&#x20;**&#x4D;oduł zasilający 230 V AC → 5 V DC.



Tranzystor BC337 jest podłączony następująco:



**-&#x20;**\*\*Baza\*\* — GPIO4 ESP32-C3 przez rezystor 10 kΩ.

**-&#x20;**\*\*Kolektor\*\* — wejście IN modułu przekaźnika.

**-&#x20;**\*\*Emiter\*\* — masa układu.



Masy ESP32-C3 i modułu przekaźnika muszą być połączone.



Dzięki tranzystorowi sygnał HIGH z GPIO4 powoduje wysterowanie wejścia LOW przekaźnika.



Układ na tyle prosty, że chyba nie muszę go rozrysowywać. :)







**## Zasada działania całego systemu**



Po uruchomieniu konfiguracji Master wyszukuje Slave przez BLE i uwierzytelnia go przy użyciu wspólnego klucza szyfrowania. Następnie przekazuje mu nazwę i hasło sieci Wi-Fi.



![Ustawienia sieci WiFi]\(images/master_siec.jpeg){width=400}

![Ustawienia sieci BLE slave]\(images/siec_slave.jpeg){width=400}



Po połączeniu z siecią Slave uruchamia serwer TCP, a Master nawiązuje z nim szyfrowaną sesję komunikacyjną.



Master cyklicznie odczytuje temperaturę z BME280. Gdy spadnie ona poniżej temperatury zadanej pomniejszonej o histerezę, wysyła polecenie włączenia ogrzewania. Gdy przekroczy temperaturę zadaną powiększoną o histerezę, wysyła polecenie wyłączenia.



Slave wykonuje polecenia, sterując przekaźnikiem, i odsyła potwierdzenia. Master wyświetla stan połączenia i poziom sygnału Wi-Fi.



W razie zerwania komunikacji Slave realizuje wcześniej skonfigurowany tryb awaryjny. Dzięki temu sterowanie ogrzewaniem nie zależy od ciągłej dostępności Mastera.



![Ustawienia przekaźnika / Trybu awaryjnego]\(images/master_przekaznik_awaria.jpeg){width=400}



Całośc obudów składa się z takich wydruków na drukarce 3D





![Wydruki 3D]\(images/wydruki_3D.jpg){width=400}



**## Pierwsze uruchomienie**



**1.&#x20;**&#x57;grać oprogramowanie Mastera i Slave z identycznym kluczem w \`SecureLink.h\`.

**2.&#x20;**&#x57;łączyć oba urządzenia w zasięgu stabilnej komunikacji Bluetooth.

**3.&#x20;**&#x4E;a Masterze uruchomić konfigurację sieci.

**4.&#x20;**&#x50;o wykryciu i uwierzytelnieniu Slave wybrać sieć Wi-Fi i wprowadzić hasło.

**5.&#x20;**&#x50;o zakończeniu konfiguracji urządzenia komunikują się już przez lokalną sieć Wi-Fi.



BLE jest wykorzystywane do konfiguracji, a nie do normalnego sterowania kotłem.



**## Home Assistant i iPhone**



Termostat współpracuje z Home Assistant przez integrację HomeKit Device lub bezpośrednio z aplikacją Dom na iPhonie.



![Termostat w Home Assistant]\(images/ha3.png){width=400}



![Panel termostatu w Home Assistant]\(images/HA1.png){width=400}



![Ustawienia termostatu w Home Assistant]\(images/HA2.png){width=400}



**---**



\*\*Uwaga:\*\* styki przekaźnika należy podłączać wyłącznie do odpowiedniego wejścia sterującego kotła, zgodnie z dokumentacją producenta. Nie należy podawać napięcia na wejście przeznaczone do sterowania beznapięciowego. Część zasilana napięciem 230 V wymaga bezpiecznego montażu i odpowiedniej izolacji.



**------------------------------------------**



**## Kompilacja i wgrywanie firmware (Windows 11 / macOS)**



Projekt wykorzystuje \*\*Visual Studio Code + PlatformIO\*\*. Nie wymaga instalowania Arduino IDE.



**### 1. Przygotowanie środowiska**



**1.&#x20;**&#x5A;ainstaluj [Visual Studio Code]\(https\://code.visualstudio.com/).

**2.&#x20;**&#x57; VS Code otwórz \*\*Extensions\*\* i zainstaluj rozszerzenie \*\*PlatformIO IDE\*\*.

**3.&#x20;**&#x55;ruchom ponownie VS Code, jeśli będzie to wymagane.

**4.&#x20;**&#x4F;twórz katalog projektu zawierający plik \`platformio.ini\`.



PlatformIO automatycznie pobierze framework ESP32, kompilator oraz biblioteki określone w konfiguracji projektu. Pierwsza kompilacja wymaga dostępu do internetu.



**### 2. Konfiguracja klucza szyfrowania**



Przed kompilacją wygeneruj własny, 32-bajtowy klucz AES-256 zgodnie z wcześniejszą instrukcją.



Umieść \*\*identyczny klucz\*\* w plikach \`SecureLink.h\` Mastera i Slave. Nie publikuj rzeczywistego klucza w repozytorium.



**### 3. Kompilacja Mastera (NiceMCU ESP32)**



**1.&#x20;**&#x4F;twórz główny katalog projektu w VS Code.

**2.&#x20;**&#x57; PlatformIO wybierz \*\*Project Tasks → Build\*\* lub kliknij ikonę \`✓\` na dolnym pasku.

**3.&#x20;**&#x50;o zakończeniu kompilacji powinien pojawić się komunikat \`SUCCESS\`.



Firmware znajduje się w:



\`\`\`text

.pio/build/\<nazwa_środowiska>/firmware.bin

\`\`\`



**### 4. Kompilacja Slave (ESP32-C3 SuperMini)**



**1.&#x20;**&#x4F;twórz katalog \`TermReg-Slave\` jako osobny projekt PlatformIO.

**2.&#x20;**&#x53;prawdź zgodność klucza AES-256 z Masterem.

**3.&#x20;**&#x55;ruchom \*\*Project Tasks → Build\*\*.



**### 5. Wgrywanie oprogramowania**



**1.&#x20;**&#x50;odłącz ESP32 do komputera przewodem USB obsługującym transmisję danych.

**2.&#x20;**&#x57; PlatformIO wybierz \*\*Project Tasks → Upload\*\*.

**3.&#x20;**&#x50;oczekaj na komunikat \`SUCCESS\`.



Jeżeli PlatformIO nie wykrywa portu automatycznie, można wskazać go w \`platformio.ini\`.



Windows 11:



\`\`\`ini

upload_port = COM5

\`\`\`



macOS:



\`\`\`ini

upload_port = /dev/cu.usbserial-XXXX

\`\`\`



Nazwy portów są przykładowe. Zależnie od płytki może być potrzebny sterownik USB-UART (np. CH340 lub CP210x).



**### 6. Diagnostyka**



Podgląd komunikatów urządzenia jest dostępny przez \*\*PlatformIO → Project Tasks → Monitor\*\*.



Diagnostykę programu można włączyć przez ustawienie \`DIAG=1\` w konfiguracji kompilacji.



\*\*Ważne:\*\* Master i Slave kompiluje się oraz programuje oddzielnie. Oba moduły muszą mieć identyczny klucz szyfrowania.



**------------------------------**

Całość testowana na piecach MPM Wood Pell 14KW ze sterownikiem ecoMAX850 i Sztoker Pellet 12KW ze sterownikiem mini-PID  



*>* ❤️ \*\*P.S. Szczególne podziękowania dla mojej kochanej siostry Magdaleny\*\* za udostępnienie pieca Sztoker Pellet i skrupulatne przeprowadzanie testów! 😉


