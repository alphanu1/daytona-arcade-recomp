# Daytona på Dreamcast frametider og hukommelsesadgang

Baseline fra 11. oktober 2026, før rettelse af alignment i geometriens tabeller.
Den almindelige frametid er cirka **18,0 ms**. Målingerne peger især på CPU'ens
geometri- og hukommelsesarbejde; PVR-renderingen ligger omkring 8,8 ms i det
målte vindue. Der mangler cirka 2 ms i gennemsnit for at komme under 16 ms.

Den nye prioritet er gameplay med en reduceret visning. Fire attract-kørsler
med biler, vej og bander ligger omkring 16,67 ms; to målinger af faktiske PVR
billedskift gav ét skift pr. vblank i hele det målte vindue. Det er et andet
forløb end race-baselinen, ikke en direkte A/B-sammenligning. Se den nye tabel
sidst i rapporten.

## Målegrundlag og begrænsninger

14 kørsler på Dreamcast-hardware: to almindelige kørsler og to gentagelser
af hver af seks hardwarehændelser. Alle gennemførte 1800 ticks uden tabte
ticks eller renderfejl og returnerede 0 til loaderen. Terminalsignaturerne for
race, silent events og input stemte overens; dette beviser ikke identiske
mellemtilstande eller kameraposer i alle frames.

Konfigurationen var autostartet race med resident teksturer, direkte SQ,
bilinear filtering på OP, uden FSAA og uden reduceret draw distance.
Omgivelserne brugte den eksisterende pakke med konsoliderede cutouts, ikke
eksperimentet med ekstra DNM5-strips. Den effektive native compilerindstilling
var O3; rendering og geometri brugte fast-math med finite-input-guards bevaret.

PRFC0 beholdt KOS' aktive CPU-timer. PRFC1 målte én hændelse ad gangen:
operandadgange, data-cache misses, instruktions-cache misses samt stall-cyklusser
fra data-cache, instruktions-cache og FPU. Se [KOS' performance counters](https://kos-docs.dreamcast.wiki/group__perf__counters.html).

De detaljerede målinger sampler hver 32. frame efter 64 opvarmningsframes:
51 måleframes pr. instrumenteret kørsel, 612 i alt. De instrumenterede
kørsler lå på 18,541–18,564 ms i gennemsnit. Aflæsningerne påvirker især små
områder omkring hvert face og hver trekant. Detaljerede tider nedenfor er
derfor **aktive CPU-tider på instrumenterede måleframes**, ikke en præcis
nedbrydning af den almindelige callbacktid på 12,133 ms.

CPU og GPU overlapper. Inclusive totals må ikke lægges sammen med deres
underområder. Missandele bygger på separate kørsler med misses og
operandadgange og er estimater. Stall-hændelser er omregnet med 200 cyklusser
pr. mikrosekund; de må ikke lægges sammen eller læses som garanteret tid,
der kan fjernes. CPU-tællerne måler ikke PVR-texturcache eller VRAM-misses.

## Den almindelige frame

Tallene kommer fra de to kørsler uden de detaljerede cachemålinger.

| Område | Gennemsnit | Bemærkning |
|---|---:|---|
| Hele framen | **17,998 ms** | Gentagelser: 17,998 / 17,997 |
| Gameplay og simulation | 3,567 ms | Eksisterende samlet måleområde |
| Kamera, modeldele og scenarieopsætning | 0,201 ms | Før renderkøen udføres |
| Renderingens kø-/objektforberedelse | 0,390 ms | Før callbacks |
| Model-rendercallbacks | **12,133 ms** | Geometriforberedelse og SQ-afsendelse |
| Rest uden særskilt måler | ≈1,71 ms | Bl.a. øvrige callbacks, engine/input/ventetid; ikke automatisk GPU-ventetid |
| PVR-rendering | **8,817 ms** | Overlapper CPU-arbejdet; må ikke lægges oveni |
| PVR-rendering, højeste observerede | 13,296 ms | Første 596 frames pr. kørsel |
| Hele framen, højeste observerede | 29,679 ms | Langsomme enkeltframes er stadig et problem |

GPU-gennemsnittene var 8,808 og 8,825 ms. Hele framens maksima var 29,679 og
27,842 ms. GPU-vinduet dækker ikke hele banen eller hele kørslen.

## Detaljeret CPU rendering

Gennemsnit af de seks hændelseskonfigurationer med to gentagelser hver.
Tiderne inkluderer instrumentering og eventuelle interrupts i intervallerne.

| Måleområde | Biler | Omgivelser | Samlet |
|---|---:|---:|---:|
| Transformation, projektion og pointcache | 0,820 ms | **2,495 ms** | 3,315 ms |
| Lighting og facing | **1,349 ms** | 0,806 ms | 2,154 ms |
| Materialevalg, vertexmarkering og facekø | 0,767 ms | 1,058 ms | 1,824 ms |
| Clipping og trekantklassifikation | 0,405 ms | **1,716 ms** | 2,122 ms |
| Gruppegrænser og normalkegler | 0,024 ms | 0,223 ms | 0,247 ms |
| Header-/vertexafsendelse til SQ | 1,229 ms | **1,831 ms** | 3,060 ms |
| Objektgrænser, view og batchoprettelse | 0,242 ms | 0,034 ms | 0,276 ms |
| Nulstilling af pointcachens status | 0,210 ms | **0,622 ms** | 0,832 ms |
| Materiale-frame-begin | — | 0,004 ms | 0,004 ms |
| Headerklargøring | Ikke kaldt | Ikke kaldt | Resident vej |
| Verdens-texturklargøring | — | Ikke kaldt | Resident vej |
| Bil-textur-require | Ikke kaldt | — | Resident vej |
| Callback-rest: udækket arbejde og måleoverhead | 1,254 ms | 2,052 ms | 3,307 ms |
| **Callbacktotal, inklusive underområder** | **6,058 ms** | **10,802 ms** | **16,860 ms** |

Callbacktotalen inkluderer transformation, lighting, materialevalg, clipping,
gruppegrænser, SQ-afsendelse og pointcache-nulstilling. Objektforberedelsen
ligger uden for callbacktotalen. Callback-resten er totalen minus de målte
underområder, ikke en ren måling af nyttigt looparbejde. Den ydre dispatchløkke
for modstanderbiler er ikke med i hvert objekts callbacktotal.

Clipping indeholder både near-plane og X/Y-behandling; z-clipping er ikke
isoleret. Lighting indeholder også facing/backface-arbejde, så hele rækken er
ikke genberegning af scenariets lysfarver. Headerklargøring inkluderer normalt
bil-textur-require, men begge springes over i den resident gameplayvej.

## Cache misses og stalls for biler

Antal og tider er pr. instrumenteret måleframe. D er data-cache, I er
instruktions-cache. D-missandel er misses divideret med separat målte
operandadgange, ikke en præcis same-frame-rate.

| Område | D-cache misses | D-missandel | D-stall µs | I-cache misses | I-stall µs | FPU-stall µs |
|---|---:|---:|---:|---:|---:|---:|
| Transformation/projektion/cache | 1.434 | 3,3 % | 153 | 161 | 19 | 82 |
| Lighting/facing | 2.355 | 4,1 % | **366** | 153 | 23 | 85 |
| Materialer/vertexmarkering/kø | 906 | 2,2 % | 152 | 150 | 22 | 3 |
| Clipping/klassifikation | 203 | 1,0 % | 59 | 91 | 11 | 2 |
| Gruppegrænser/normalkegler | 81 | 14,5 % | 10 | 22 | 4 | <1 |
| SQ-afsendelse | 1.989 | 2,6 % | **285** | 126 | 19 | 41 |
| Objektgrænser/view/batches | 201 | 1,5 % | 24 | 205 | 25 | 20 |
| Pointcache-nulstilling | 1.541 | **32,5 %** | 139 | 8 | 2 | <1 |
| Callback-rest | 2.172 | 3,7 % | 355 | 322 | 46 | <1 |
| **Callbacktotal, inklusive underområder** | **10.681** | **3,5 %** | **1.518** | **1.034** | **143** | **213** |

Bilernes frame-begin, headerklargøring og textur-require gav ingen live
målinger i denne resident konfiguration.

## Cache misses og stalls for omgivelser

| Område | D-cache misses | D-missandel | D-stall µs | I-cache misses | I-stall µs | FPU-stall µs |
|---|---:|---:|---:|---:|---:|---:|
| Transformation/projektion/cache | **5.144** | 4,7 % | **598** | 226 | 27 | 256 |
| Lighting/facing | 878 | 2,7 % | 171 | 134 | 18 | 112 |
| Materialer/vertexmarkering/kø | 2.271 | 4,4 % | 315 | 108 | 17 | 3 |
| Clipping/klassifikation | 1.123 | 1,3 % | 179 | **1.150** | **110** | 84 |
| Gruppegrænser/normalkegler | 229 | 2,0 % | 28 | 201 | 23 | 19 |
| SQ-afsendelse | 2.829 | 2,6 % | **401** | 127 | 19 | 84 |
| Objektgrænser/view/batches | 62 | 3,6 % | 7 | 31 | 4 | 3 |
| Pointcache-nulstilling | **3.626** | **33,1 %** | **456** | 25 | 4 | <1 |
| Materiale-frame-begin | 5 | 8,3 % | 2 | 8 | 1 | <1 |
| Callback-rest | 4.255 | 5,0 % | 637 | 1.188 | 146 | <1 |
| **Callbacktotal, inklusive underområder** | **20.355** | **4,1 %** | **2.783** | **3.159** | **363** | **557** |

Headerklargøring og verdens-texturklargøring gav ingen live målinger i den
resident vej. Callbacktotalerne viser tilsammen cirka 4,30 ms D-stall mod
0,51 ms I-stall og 0,77 ms FPU-stall på måleframes. Det peger på dataadgang
som et oplagt fokus før yderligere matematisk mikrooptimering alene, men
tallene er ikke additive besparelser.

## Hukommelsesadgang i de samme områder

SH4 har direkte mappede caches: 16 KiB data og 8 KiB instruktioner med
32-byte linjer. Alignment forhindrer ikke i sig selv kapacitets- eller
adressekonflikter. Se [Renesas' hardwaremanual, tabel 4.1](https://www.renesas.com/en/document/mah/sh7750-sh7750s-sh7750r-group-users-manual-hardware).

| Måleområde | Faktisk adgangsmønster | Cachemæssig betydning |
|---|---|---|
| Transformation/projektion/cache | Sekventiel scan af 32-byte pointentries; 16-byte sourcevertices; indekserede opslag i positionsrepræsentanter | Blanding af streaming og gathers; dubletter kopierer tidligere cacheentries |
| Lighting/facing | Sekventielle faces, men indekserede sourcevertices og materialer; normaler fra faces | Genlæser original geometri, ikke blot den transformerede pointcache |
| Materialer/vertexmarkering/kø | Materiale/luma-opslag; spredte statuswrites til pointcache; kompakte 4-byte facekø-entries | Flere datasæt konkurrerer om cachen; køskrivningen er kompakt, vertexmarkeringen er ikke |
| Clipping/klassifikation | Indekserede læsninger af typisk tre pointentries; klippede polygoner bruger stackbuffer med 20-byte entries | Intakte trekanter undgår klippebufferen; vanskelig geometri giver mere kode og midlertidige data |
| Gruppegrænser/normalkegler | Gruppevise læsninger af bounds/kegler og små statuswrites | 64-byte normalgrupper; den aktuelle world-tabel starter 16 mod 32 |
| SQ-afsendelse | Pointgathers, materialebinding, UV-cache og headerpointer; direkte writes til SQ | Ingen lokal framebuffer af færdige vertices; inputdata læses stadig gennem CPU-cachen |
| Objektgrænser/view/batches | Instance-scan, mesh-opslag og oprettelse af 64-byte batchdata | Instances er 112 bytes; hver entry er derfor ikke 32-byte aligned |
| Pointcache-nulstilling | Ét 4-byte statuswrite pr. 32-byte entry, sekventielt gennem hele batchens pointbuffer | Lav nyttig datamængde pr. berørt linje; høj målt missandel |
| Materiale-frame-begin | Lille resident bookkeeping | Ikke en texturopload; næppe et relevant optimeringsmål |
| Headerklargøring | Springes over live; headere kompileres ved startup | Renderingen læser stadig de cachede headere under SQ-afsendelse |
| Verdens-texturklargøring | Springes over live i denne resident konfiguration | Ingen runtime-upload/eviction i forløbet |
| Bil-textur-require | Springes over live efter residency | Runtime upload/decode/fence-tællere var nul |
| Callbacktotal/rest | Kombination af ovenstående samt udækkede opslag, løkker og profilerarbejde | Resten er endnu ikke præcist fordelt på operationer |

Pointbufferen får to fulde scans pr. forberedt batch: først nulstilling,
senere demand/transformation. Imellem dem markerer faces vertices med
indekserede writes. OP/PT deler den forberedte batch, så begge lister kræver
ikke hver sin transformation af alle points.

Bilernes 32-byte faces og materialer starter 16 bytes inde i en cachelinje.
Omgivelsernes 64-byte normalgrupper har samme forskydning. Dette er
layoutfejl, der skal rettes, ikke accepterede permanente layouts.
Den aligned filbuffer garanterer ikke alignment af tabellerne inden i den.
De 32-byte pointcacheentries er derimod aligned.

Der er ingen eksplicit data-prefetch i den undersøgte live direkte-SQ-vej.
SQ bruger PREF til at sende store queues; det er ikke prefetch af geometri.
Den fundne cache-line-allocation i den gamle midlertidige trekantvej gælder
startup-oraklet, ikke den aktuelle gameplayafsendelse.

Pointcachekapaciteten er cirka 1,12 MB for biler og 504 KB for omgivelser.
Det er allokeret kapacitet, ikke nødvendigvis mængden berørt i hver frame,
men det er langt mere end data-cachen. Live instances er adskilt fra de
immutable pack-instances; deres 112-byte stride og allokering garanterer
ikke 32-byte alignment for hver entry.

## Rådata og reproduktion

Alle rå ns-/eventtotaler, gentagelser, ELF-hashes, de 143 monterede assets'
hashes og registrerede source-hashes findes i
[summary.json](../build/dc-native-render-pmu.SnkQ4d/summary.json).
Konfiguration, rækkefølge og kontrolgrundlag findes i
[evidence.txt](../build/dc-native-render-pmu.SnkQ4d/evidence.txt).
Artefakterne ligger lokalt i den ignorerede build-mappe; rapporten alene
indeholder ikke rådataene. Asset- og source-manifestet er registreret efter
kørslerne, ikke som en før/efter-kontrol af samtidige ændringer.

```sh
python3 build/dc-native-render-pmu.SnkQ4d/analyze.py 14
```

Kommandoen kontrollerer antal kørsler, exit/resultater, terminalsignaturer,
eventvalg, sampleantal, scope-ejerskab og de registrerede ELF-identiteter.
Profilerens accumulator-kontrol og SQ-regression består, inklusive checks
for 48-bit event-wrap, deaktiveret sampling og uændrede header-/vertexbytes.

## Fokus efter denne baseline

Alignment i packer og læser skal rettes før næste performance-sammenligning.
Derefter er omgivelsernes pointcache-/transformationsforløb et oplagt første
område, fulgt af SQ-afsendelsens opslag og clipping. Ingen gevinst fra disse
ændringer er bevist af denne baseline.

Cacheadfærd i gameplay, PVR-texturcache samt fuld bane og intro er ikke dækket
af de detaljerede målere her. Instrumenterede CPU-tider må ikke bruges som
præcise normale frameomkostninger.

## Tabelalignment efter baseline

En isoleret offline ændring retter bilernes face-/materialetabeller og
omgivelsernes normalgrupper til 32-byte tabelstart. Hele tabelindholdet,
vertex-/face-rækkefølge, UV'er, materialer og normaler er byte-identiske.
Pakkerne vokser tilsammen kun 50 bytes: bilpakken med 34, scenariet med 16.
Loaderens rounded allokering vokser 32 bytes for bilpakken og er uændret for
omgivelserne: 1.100.992 → 1.101.024 bytes og 467.968 → 467.968 bytes.

Packerens afsluttende `--align-tables`-pass markerer det nye layout eksplicit.
Loaderen accepterer både det gamle og det nye layout og kontrollerer nul-padding
ved alle tabelgrænser og EOF. Funktionen ændrer ikke record-strides:
16-byte sourcevertices, 112-byte instances og historiske 24-byte DNM3-grupper
er ikke individuelt 32-byte aligned. De aktuelle 32-byte faces/materialer og
64-byte normalgrupper er det. Dette er en tabelrettelse, ikke afslutningen på
arbejdet med alle datastrukturer. Det nye layout er endnu et eksplicit packer-valg;
baseline-assets er ikke overskrevet.

### Frametider med og uden aligned tabeller

Fire almindelige hardwarekørsler i rækkefølgen gammel, aligned, aligned,
gammel; samme ELF og kun `car.dnm`/`world.dnm` ændret mellem mountmapperne.

| Område | Gamle tabeller | Aligned tabeller |
|---|---:|---:|
| Hele framen, gennemsnit af to kørsler | 17,970 ms | 17,963 ms |
| Hele framen, gentagelser | 17,972 / 17,968 ms | 17,965 / 17,961 ms |
| Rendercallbacks, gennemsnit | 12,150 ms | 12,138 ms |
| PVR-rendering, gennemsnit i samplevinduet | 8,825 ms | 8,816 ms |
| Højeste hele frame observeret | 30,301 ms | 30,009 ms |

Forskellen på 7 µs i samlet gennemsnit viser ingen mærkbar frametidsgevinst.
Under 16 ms er stadig ikke opnået. GPU-tiden overlapper CPU og dækker fortsat
kun de første 596 frames pr. kørsel.

### Data-cache efter tabelrettelsen

Yderligere fire hardwarekørsler: ét gammelt/aligned par for data-cache misses
og ét par for data-cache stalls. Hvert par bruger sin egen identiske ELF.
Hver kørsel har 51 instrumenterede måleframes. Tallene kan ikke sammenlignes
direkte med den tidligere seks-event-baseline eller bruges som quiet CPU-tider.

| Måleområde | D-misses gammel | D-misses aligned | D-stall gammel | D-stall aligned |
|---|---:|---:|---:|---:|
| Bilcallbacks inklusive underområder | 8.654 | 8.506 | 1.269 µs | 1.261 µs |
| Omgivelsescallbacks inklusive underområder | 20.417 | 20.733 | 2.856 µs | 2.888 µs |

Der er ingen samlet nedgang i de målte misses eller stalls i dette ene par
pr. event. Det betyder ikke, at alignment er uvigtig, men denne ændring
forklarer ikke de manglende millisekunder. Kørslernes sluttilstande stemmer
overens; mellemframes, synlig geometri og kamerainterpolation er ikke bevist
identiske. Derfor er små forskelle ikke sikre årsagsbeviser.

Alle otte kørsler gennemførte 1800 ticks med dropped/failed/SQ-failed nul og
returnerede 0 til loaderen. Startup-SQ-orakler og transform-/lyskontroller
bestod med begge layouts. Seks packer-regressioner, den nye alignment-check
med ASan/UBSan og 556 afviste inputs samt de eksisterende SQ-/profilchecks
består. Bilernes materialekontaktark og metadata er byte-identiske før/efter.
Dette dækker ikke et helt race eller en fuld visuel sammenligning på HDMI.

Rådata, hashes, tabeloffsets, buildlogs og mountmanifest findes i
[alignment summary.json](../build/dc-native-align-tables.cRiCBG/summary.json).
Kontrollen kan genkøres med:

```sh
python3 build/dc-native-align-tables.cRiCBG/analyze.py 8
```

## Pointcache uden fuld nulstilling

Den direkte SQ-vej nulstiller ikke længere alle pointstatusser før hvert objekt.
Status initialiseres én gang ved allokering. Synlige faces markerer aktuelle
corners som krævede; delte XYZ-repræsentanter får kun en ugyldig status, hvis
de ellers kunne levere en gammel projektion. Koordinater for ubrugte punkter
forbliver urørte. Den eksisterende stigende transformationsrækkefølge,
inside-bevis, aliasernes egne UV'er og 32-byte entries er bevaret.
Transformationspasset scanner stadig pointtabellen; det er kun det fulde
nulstillingspass, som er væk. Startup-referencevejen beholder sin nulstilling.

### Almindelige frametider efter ændringen

Fire hardwarekørsler i rækkefølgen gammel, ny, ny, gammel. Alle bruger samme
143 aligned assets og buildindstillinger, men ændringen kræver to forskellige
ELF'er. Alle gennemførte 1800 ticks uden dropped/failed/SQ-failed og returnerede
0 til loaderen. Deres terminalsignaturer stemmer overens.

| Område | Fuld nulstilling | Målrettet markering |
|---|---:|---:|
| Hele framen, gennemsnit af to kørsler | 17,959 ms | 17,714 ms |
| Hele framen, gentagelser | 17,969 / 17,949 ms | 17,715 / 17,713 ms |
| Rendercallbacks, gennemsnit | 12,137 ms | 11,843 ms |
| PVR-rendering, første 596 frames | 8,821 ms | 8,826 ms |
| Højeste hele frame observeret | 30,360 ms | 29,341 ms |

Nettoforskellen er 0,245 ms pr. frame i dette forløb, cirka 1,4 procent.
Under 16 ms er endnu ikke opnået. Den tidligere instrumenterede
nulstillingstid på 0,832 ms er ikke en lovet almindelig frametidsbesparelse:
markeringen af repræsentanter koster også arbejde, og profilerens overhead
og cachepåvirkning er forskellige. ELF'ernes kodeplacering er ikke isoleret.
Antallet af renderframes varierer fra 1743/1745 til 1768/1768; 1800 simulationsticks
og ens terminaltilstand beviser ikke ens interpolation eller synlige mellemframes.

Den faktiske allokering, forberedelse og OP/PT-submission består fokuserede
hostkontroller med ASan/UBSan: gamle inside/outside/invalid-repræsentanter,
ændret pose/view, udvalgte faces, egne UV'er og genbrug af andre objekters gamle
bufferslices. Ubrugte canary-poster forbliver urørte; demand-tags forbruges.
En mutation uden repræsentant-invalideringen afvises ved sammenligning med
friskberegnede punkter. Eksisterende profiled/unprofiled SQ-byteparitet på
13.900 bytes og fuld native `make check` består. Hostadapteren beviser ikke
SH4-matematik eller fuld visuel racekorrekthed.

### CPU og cache efter fjernelse af nulstilling

Ét før/efter-par pr. event: event 15 for D-misses, event 37 for D-stall-cyklusser.
Gammel version har 51 samples pr. kørsel; ny har 52. Tallene er gennemsnit pr.
måleframe, ikke totale kørselsværdier. Tiderne nedenfor kommer kun fra event
37-parret og inkluderer instrumentering; de er ikke almindelige callbacktider.
Kameraposer i måleframes er ikke autentificeret ens. En enkelt parvis måling
er ikke et statistisk bevis for de enkelte underområders ændringer.

| Område | Tid før | Tid efter |
|---|---:|---:|
| Biler, nulstilling | 214,4 µs | 0 µs |
| Biler, materialer og vertexmarkering | 770,4 µs | 1.104,5 µs |
| Biler, transformation og pointcache | 825,0 µs | 831,5 µs |
| Biler, callbacks inklusive underområder | 5.992,6 µs | 6.158,4 µs |
| Omgivelser, nulstilling | 631,7 µs | 0 µs |
| Omgivelser, materialer og vertexmarkering | 1.214,8 µs | 1.459,2 µs |
| Omgivelser, transformation og pointcache | 2.600,3 µs | 2.648,1 µs |
| Omgivelser, callbacks inklusive underområder | 11.433,5 µs | 10.895,3 µs |

| Samme område | D-misses før | D-misses efter | D-stall før | D-stall efter |
|---|---:|---:|---:|---:|
| Biler, nulstilling | 1.569 | 0 | 141,2 µs | 0 µs |
| Biler, materialer og vertexmarkering | 922 | 2.036 | 153,8 µs | 270,4 µs |
| Biler, transformation og pointcache | 1.440 | 1.694 | 153,2 µs | 181,6 µs |
| Biler, callbacks inklusive underområder | 8.507 | 8.192 | 1.259,2 µs | 1.229,2 µs |
| Omgivelser, nulstilling | 3.655 | 0 | 463,0 µs | 0 µs |
| Omgivelser, materialer og vertexmarkering | 2.292 | 3.202 | 320,8 µs | 419,9 µs |
| Omgivelser, transformation og pointcache | 5.850 | 6.008 | 696,0 µs | 754,5 µs |
| Omgivelser, callbacks inklusive underområder | 20.626 | 16.227 | 2.887,5 µs | 2.313,0 µs |

Nulstillingspasset er væk, men den målrettede repræsentantmarkering øger arbejdet
i materialescope. De inclusive rækker må ikke summeres med deres underområder;
stalltid er heller ikke en ekstra tidskategori, der skal lægges til CPU-tiden.
Bilernes instrumenterede callbacktid stiger i dette par, mens omgivelsernes
falder. Den samlede almindelige før/efter-måling ovenfor er grundlaget for
frametidsgevinsten, ikke en sum af isolerede scopes.

Alle otte gennemførte hardwarekørsler har 1800 ticks og ingen fejl. Én loader-STAT
timede ud før upload; ingen ELF blev startet ved det forsøg, og ingen reset blev
brugt. Efter en frisk vellykket STAT blev det manglende event-par gennemført.
Konsollen er tilbage i loaderen. Samtlige 143 asset-hashes og de registrerede
source-hashes stemmer efter kørslerne. Rå totaler, ELF-hashes og konfiguration:
[pointcache summary.json](../build/dc-native-no-point-reset.SgrEKa/summary.json)
og [evidence.txt](../build/dc-native-no-point-reset.SgrEKa/evidence.txt).

```sh
python3 build/dc-native-no-point-reset.SgrEKa/analyze.py 8
```

## Vurdering af OCRAM til rendering

SH4 kan dele sine 16 KiB operand-cache i 8 KiB almindelig data-cache og
8 KiB adresserbar on-chip RAM. Adgang til denne scratchpad giver ikke cache
misses, men flytning fra SDRAM og adgang til alle andre datasæt koster stadig.
Med OIX slået fra er `0x7c001000` et sammenhængende 8 KiB vindue.
Se [Renesas hardwaremanual, afsnit 4.3.6](https://www.renesas.com/en/document/mah/sh7750-sh7750s-sh7750r-group-users-manual-hardware?r=1055166).

Vores installerede KOS tilbyder `dcache_toggle_ocram()` og `__ocram`.
Den underliggende `cache_write_ccr()` blokerer interrupts, kører skiftet fra
uncached P2, flusher/invaliderer alle 512 data-cachelinjer og venter otte
instruktioner efter CCR-skrivningen. Det er ikke kun et billigt register-bitflip;
writeback og efterfølgende kolde cacheopslag skal med i nettoomkostningen.
Se den lokale [KOS-implementering](/opt/toolchains/dc/kos/kernel/arch/dreamcast/kernel/cache.s:147)
eller [KOS-kilden](https://github.com/KallistiOS/KallistiOS/blob/master/kernel/arch/dreamcast/kernel/cache.s).

Hele pointcachen kan ikke flyttes dertil: 8 KiB rummer højst 256 af vores
32-byte entries og ingen anden metadata. Kompakte offline geometri-klumper
med eksempelvis 128 lokale punkter bruger 4 KiB til transformerede punkter;
resten kan bruges til clipping og øvrig arbejdsdata. Det kræver lokal
indeksering og en entydig levetid for scratchdata, især på tværs af OP/PT.
Mindre clipping-buffere er en enklere kandidat, men deres gevinst er ukendt.

Et afgrænset forsøg bør først måle skift + writeback + cachegenopvarmning,
dernæst normal cache mod OCRAM med samme arbejdsdata og endelig hele framen.
Skift pr. polygon eller bil er ikke anbefalet. Permanent OCRAM eller ét samlet
renderingsinterval er kandidater, ikke dokumenterede forbedringer. Der er
endnu ingen OCRAM-kode eller OCRAM-hardwarebenchmark i denne port.

## Kandidater til målrettet prefetch

SH4ZAM har `SHZ_PREFETCH(ptr)`, som på SH4 bliver `pref @Rn`. Instructionen
henter en 32-byte cachelinje på forhånd; den skjuler ventetid, men gør ikke
data permanent resident. Renesas beskriver, at prefetch af allerede cachede
data ikke gør noget. Se [hardwaremanual, afsnit 4.3.9](https://www.renesas.com/en/document/mah/sh7750-sh7750s-sh7750r-group-users-manual-hardware?r=1055166).

Den aktuelle SQ-vej har ikke eksplicit prefetch af geometri. SH4ZAMs matrixloader
har allerede intern prefetch; SQ's `PREF` sender vertexdata og er en anden
anvendelse af instructionen. Vi skal derfor ikke tælle SQ-afsendelsen som
prefetch af kommende sourcevertices.

| Renderingområde | Forudsigelig adgang | Muligt lookahead |
|---|---|---|
| Faceselektion og vertexmarkering | Sekventielle 32-byte faces, derefter indekserede corners/repræsentanter | Hent kommende faces; brug deres indices til at hente kommende pointstatusser |
| Transformationsscan | Lineære 32-byte pointentries og 16-byte sourcevertices, men kun krævede punkter transformeres | Hent kommende pointlinjer og sourcevertex-linjer med særskilt afstand |
| SQ-afsendelse | Kompakt facekø, derefter indekserede faces, headere og transformerede punkter | Kig frem i køen og hent de punkter, som kommende faces faktisk bruger |

Et isoleret forsøg skal variere få lookahead-afstande, bevare byte-identisk
geometriafsendelse og måle både det relevante scope og hele framen. Unødvendige
prefetches kan koste busbåndbredde og fortrænge brugbare cachelinjer, især ved
lav andel synlige faces. Ingen prefetch-afstand eller gevinst er endnu målt.

## Resterende alignment i assets og live renderdata

Audit af de faktiske gamle/aligned `car.dnm` og `world.dnm` bekræfter alle
tabelbytes og offsets mod alignment-manifestet. De seneste hardwaretests bruger
aligned test-assets; gamle pakker er stadig understøttet og ikke overskrevet.

| Data | Nuværende layout | Vurdering |
|---|---|---|
| Faces og materialer | 32-byte records, 32-aligned tabelstart i testpakkerne | Rettet; gamle bilpakker havde tabelstart 16 bytes inde i linjen |
| Normalgrupper | 64-byte records, 32-aligned start | Rettet; gamle omgivelser berørte tre linjer pr. gruppe i stedet for to |
| Sourcevertices | 16-byte records, aligned tabelstart | To entries pr. linje, ingen krydser en linje; padding til 32 bytes fordobler storage |
| Positionsreferencer | Pakkede uint16, aligned tabelstart | 16 references pr. linje; entry-padding er ikke begrundet |
| Immutable instances | 112-byte stride, matrixoffset 16, matrixlængde 48 | Records/matricer starter skiftevis ved offset 0/16; matricer bruger to linjer i aligned packs |
| Live bil-instances | `alignas(32)` arraybase, 112-byte stride | Rettet efter den første layoutaudit; matricer berører nu to linjer |
| Live scenery-instances | `memalign(32)`, 112-byte stride | Eksplicit basegaranti; stride er uændret |
| Direkte SQ-pointcache | 32-byte entries og `memalign(32)` | Aligned; ingen yderligere padding nødvendig |

### Matrixoffsets før rettelsen på SH4

En lille deklarationskontrol kompileret med den faktiske SH4-toolchain og
`NATIVE_FRAME_PROFILE` giver disse offsets i `nm_practice_scene_t`. Scenen
selv er `alignas(32)`; det gør ikke dens medlemmer aligned.

| Medlem | Offset i scenen | Offset modulo 32 |
|---|---:|---:|
| `view` | 65.836 | 12 |
| `body_view` | 65.884 | 28 |
| `instance` | 65.940 | 20 |
| `car_instances` | 66.056 | 8 |

Live bilernes 48-byte matrice starter 16 bytes inde i en 112-byte instance.
Derfor starter matricerne i dette array skiftevis 24 og 8 bytes inde i en
cachelinje. Halvdelen berører tre linjer, ikke to. `body_view` berører også
tre linjer. Dette er en reel layoutkandidat, ikke blot et kosmetisk
`sizeof`-problem. SH4ZAMs `shz_xmtrx_load_3x4` prefetcher allerede ved matrix
+32, men et tredje linjeskift ved halen er ikke dækket af det lookahead.

32-byte alignment af arraybasen ville give matrixstarter 16/0 og to linjer
pr. matrix uden at ændre den 112-byte stride. At padde stride til 128 gør
derimod ikke i sig selv matrixoffsettet 16 til 32-aligned; feltlayoutet skal
også vurderes. Separate member-alignments af viewmatricer og aligned allocation
af scenery-instances er afprøvet nedenfor uden at ændre stride.

Kontrolkilde, SH4-object, rå uint32-resultater og buildlog ligger i
`build/dc-native-no-point-reset.SgrEKa/instance_layout.*` og
`instance-layout-build-fixed.log`. Dette er en layoutkontrol, ikke en
gameplay-matematiktest. Den er ikke kørt på konsollen.

### Live alignment uden mærkbar framegevinst

`view`, `body_view`, `instance` og `car_instances` har nu eksplicit 32-byte
member-alignment, kontrolleret med `offsetof`-asserts. Scenery-instances bruger
`memalign(32)`. Det målte SH4-build har offsets 65.856, 65.920, 65.984 og 66.112;
alle modulo32=0. Scenen vokser fra 77.472 til 77.536 bytes. Matricerne i det
112-byte bilarray starter skiftevis 16/0 i en linje og berører altid to linjer.

| Almindelige 1800-tick racekørsler | Før | Efter |
|---|---:|---:|
| Hele framen, to kørsler | 17,714 / 17,703 ms | 17,711 / 17,733 ms |
| Gennemsnit | 17,709 ms | 17,722 ms |
| Rendercallbacks, gennemsnit | 11,842 ms | 11,820 ms |
| GPU, første 596 samples | 8,832 ms | 8,832 ms |

Otte serialiserede kørsler inklusive ét par pr. cache-event15/37 gennemførte
1800 ticks med dropped0/failed0/SQfailed0/return0. Samme143 aligned assets og
terminale racesignaturer. De almindelige A/B/B/A-kørsler viser ingen mærkbar
framegevinst; PMU-par er instrumenterede og må ikke bruges som quiet frame-tider.
Evidens og valideret sammenstilling:
[live alignment summary](../build/dc-native-live-align.kFxK0y/summary.json).

Brugerens nye prioritet er korrekt gameplay og stabil60fps med biler, vej og
umiddelbare bander. Flere cache-/paddingeksperimenter afventer denne milepæl.

### Plads til eventuel padding

| Hypotetisk ændring i de aktuelle pakker | Ekstra bytes |
|---|---:|
| Alle sourcevertices fra 16 til 32 bytes | 808.432 |
| Alle serialized instances fra 112 til 128 bytes | 3.392 |
| Live 99 bil-instances, separat body-instance og 22 scenery-instances fra 112 til 128 | 1.952 |

Vertexpadding er cirka 790 KiB ekstra og er ikke nødvendigt for at undgå
linjekrydsning i de nuværende vertex-tabeller. Mere plads er ikke automatisk
bedre cacheadfærd: spredte opslag og scanninger berører flere linjer. En tidligere
beginner-måling havde cirka 6 MB samlet uncommitted/reusable plads, men det er
ikke et RAM-budgetbevis for hele racet eller de to andre baner. Alignment af
konkrete hotte baser/felter bør skilles fra blanket-padding af alle assets.

## Reduceret attract med vej biler og bander

Den native cinematiske sekvens bruger uændret simulation, kontaktdata og kamera.
Tegningen beholder 1.373 af 4.878 world-flader: 385 vej/markeringer, 123 kantsten
og 865 umiddelbare bander/vægge. Træer, klipper, tribuner, skilte og himmel er
skjult. Udvælgelsen udføres én gang efter valideret indlæsning og før
renderer-attach; hver frame går kun gennem de beholdte faces. Ubrugte
sourcevertices og hele teksturbanken er stadig indlæst. Den komplette
assetoptimering er udsat.

| Måling | Kørsel 1 | Kørsel 2 | PVR-kørsel 1 | PVR-kørsel 2 |
|---|---:|---:|---:|---:|
| Hele CPU-loopinterval, gennemsnit ms | 16,671 | 16,667 | 16,668 | 16,667 |
| Simulation og bookkeeping ms | 1,277 | 1,278 | 1,307 | 1,307 |
| Kamera/parts/baneselektion ms | 0,253 | 0,253 | 0,272 | 0,272 |
| Renderforberedelse før callbacks ms | 0,530 | 0,530 | 0,546 | 0,546 |
| Rendercallbacks CPU ms | 7,782 | 7,791 | 7,757 | 7,764 |
| GPU gennemsnit ms, første 596 samples | 5,766 | 5,769 | 5,753 | 5,765 |
| GPU maksimum ms, samme vindue | 9,382 | 9,373 | 9,566 | 9,247 |
| CPU-loopinterval maksimum ms | 23,706 | 23,724 | 23,752 | 23,752 |
| Billedskift / vblanks efter 64 frames | — | — | 1.814 / 1.814 | 1.814 / 1.814 |
| PVR billedskiftintervaller over 20 ms | — | — | 0 | 0 |

Hver kørsel gennemfører 1.800 ticks uden dropped simulationstid eller renderfejl,
med terminal stage1/transitions2/cars21/pairs863/responses106, og returnerer 0
til loaderen. Kameraet skifter mellem de eksisterende native stadier med 20/21
biler. De to PVR-kørsler viser cirka 60 fps og ingen mistede refresh i målevinduet.
PVR's IRQ-tidsstempler gav 16,683 ms i gennemsnit og 19,163/19,162 ms maksimum.
CPU-updateren havde 39 intervaller over 18 ms i hver kørsel efter opvarmning;
det er pipeline-/tidsudsving, ikke automatisk tabte skærmframes. CPU og GPU
overlapper, så deres tal skal ikke lægges sammen som en samlet frametid.

Hardwaremålingen beviser dette attract-forløb, ikke et helt korrekt race eller
under 16 ms i alle CPU-intervaller. Fysikkens faste 17.384 µs-tick er ikke ændret
for at opnå 60 fps-rendering. Klassifikation, retained face bytes/order, uændrede
bounds/grupper, boot og begrænset exit er kontrolleret med actual-source-tests
og ASan/UBSan. HDMI ved tick 180/409 viser biler, vej og bander uden dekoration.

[Validerede resultater](../build/dc-native-track-attract.sasfYW/summary.json),
[HDMI med biler og bander](../build/dc-native-track-attract.sasfYW/attract-2.png).
Kilde-, ELF- og testhashes ligger i samme evidensmappe. Ingen ny CDI er bygget.

## Sceneovergang og reduceret 40 bils race

Attract til race genindlæses nu korrekt. Det fælles load-sted frigiver de gamle
game-owned CPU-pakker og live world-instances, og nulstiller borrowed pointers,
før det indlæser nye pakker. Kaldere frigiver fortsat rendererressourcerne først;
enDjinns callback- og PVR-lifecycle er uændret. En ASan/UBSan-kontrol med den
faktiske load/release-kode og meshloader dækker gentagelser, tidlige/sene loadfejl,
retry og enkelt frigivelse. Den gamle variant fejler ved anden indlæsning.
GPU og startup-matematik er stubbet i denne ejerskabskontrol; den samlede native
buildkontrol og den faktiske hardwareovergang består særskilt.

Et separat hardwarediagnostic sender ét syntetisk START efter fem sekunder i
attract. Den normale produktionsgren genindlæser race-assets og gennemfører
1.800 race-ticks med dropped0/failed0/SQfailed0/return0. Det er ikke en fysisk
START-test. Overgangen har en indlæsningspause; dens sessionblandede profil-
tællere er udeladt fra performance-tabellen. Ingen runtime-renderindstilling
eller ny PVR-lifecycle er tilføjet for diagnosticet.

To separate kørsler starter direkte i det normale 40-bils-race med den samme
reducerede visning og samme ELF. Spillerens sidste registrerede steer/gas/brake
er 0/0/0 i begge; dette er startfelt/modstandertrafik, ikke aktiv kørsel hele banen rundt.

| Måling | Race 1 | Race 2 |
|---|---:|---:|
| Hele CPU-loopinterval, gennemsnit ms | 16,671 | 16,677 |
| Hele CPU-loopinterval, maksimum ms | 19,245 | 21,775 |
| Simulation og bookkeeping ms | 3,339 | 3,337 |
| Kamera/parts/baneselektion ms | 0,214 | 0,213 |
| Renderforberedelse før callbacks ms | 0,401 | 0,403 |
| CPU-update i alt ms | 3,955 | 3,955 |
| Rendercallbacks CPU ms | 6,950 | 6,951 |
| GPU gennemsnit ms, første 596 samples | 6,046 | 6,047 |
| GPU maksimum ms, samme vindue | 9,150 | 9,170 |
| Billedskift / vblanks efter 64 frames | 1.813 / 1.813 | 1.813 / 1.813 |
| PVR billedskiftinterval gennemsnit / maksimum ms | 16,683 / 19,163 | 16,683 / 19,164 |
| PVR billedskiftintervaller over 20 ms | 0 | 0 |

Begge når phase22/ticks1800/released32/clock1237/remaining1963/pairs993/
responses366 uden tabt simulationstid eller fejl og returnerer til loaderen.
Dette støtter cirka 60 fps i det målte race-startforløb. CPU og GPU overlapper;
deres tal må ikke summeres til en frametid. Et helt korrekt, inputstyret løb,
under 16 ms for alle CPU-intervaller og en sømløs sceneovergang er ikke bevist.
HDMI-kortet kunne ikke låses til denne capture; der er ingen ny visuel kontrol.

Kodeaudit på dette tidspunkt viste `proximity_disabled=1` i den aktive spillerinput.
Det springer de tre linked-neighbor-positioner og proximity-drag-opdateringen
over; race-kollisionerne køres separat og er ikke slået fra. Denne wiring er
den følgende gameplay-opgave, løst i målingen nedenfor.

[Resultater og inputidentitet](../build/dc-native-reload.FcZXUA/summary.json),
[overgangslog](../build/dc-native-reload.FcZXUA/transition-1.log),
[ejerskabskontrol](../build/dc-native-reload.FcZXUA/reload-check.log).
`check.log`, buildlogs, kildehashes og ELF-hashes ligger i samme mappe.

## Aktiv proximity og kontrolleret gasinput

Spillerens aktive raceinput har ikke længere den ubetingede proximity-bypass.
Den eksisterende shared funktion læser tre `next`-neighbors fra den native
bilring og opdaterer proximity-state, ligesom den importerede kildevej gør,
når inhibit/penalty er nul. Rolling, finish og recovery beholder deres eksisterende
gates. Bilkollisioner, matematikfunktioner, renderer og 17.384 µs-tick er uændrede.
Originalens parameter kommer fra `0x501a80`; dette er en rettelse af den normale
native inputforbindelse, ikke en påstand om, at alle originalens game states
altid har proximity aktiv.

Et separat diagnostic sender centreret styring, fuld højre trigger og nul venstre
trigger gennem enDjinns almindelige getter. Det tilsvarer steer/gas/brake 0/255/0.
Første tilsluttede controller vælges; knapper og andre controllere bevares.
Den lille hostkontrol med ASan/UBSan dækker tomme slots og gentaget input og
er med i `make check`. Den tester input-wiring, ikke gameplay-matematik.
Diagnosticet er kun linket i test-ELF'er; et separat almindeligt build har ingen
wrapper og læser fortsat fysisk input.

| Måling | Før 1 | Efter 1 | Efter 2 | Før 2 |
|---|---:|---:|---:|---:|
| Hele CPU-loopinterval gennemsnit ms | 16,670 | 16,669 | 16,672 | 16,668 |
| Hele CPU-loopinterval maksimum ms | 19,425 | 20,369 | 19,380 | 19,339 |
| Simulation og bookkeeping ms | 3,527 | 3,558 | 3,552 | 3,532 |
| Kamera/parts/baneselektion ms | 0,250 | 0,251 | 0,251 | 0,249 |
| Renderforberedelse ms | 0,454 | 0,460 | 0,459 | 0,454 |
| CPU-update i alt ms | 4,232 | 4,271 | 4,263 | 4,236 |
| Rendercallbacks CPU ms | 6,776 | 6,841 | 6,843 | 6,777 |
| GPU gennemsnit ms, første 596 samples | 6,062 | 6,051 | 6,044 | 6,071 |
| GPU maksimum ms, samme vindue | 9,199 | 8,833 | 9,176 | 9,140 |
| Billedskift / vblanks efter 64 frames | 1.814 / 1.814 | 1.814 / 1.814 | 1.813 / 1.813 | 1.814 / 1.814 |
| PVR billedskiftintervaller over 20 ms | 0 | 0 | 0 | 0 |
| Spillerens slutprofil / speed-index | 141 / 247 | 145 / 252 | 145 / 252 | 141 / 247 |
| Collision pairs / responses | 1.026 / 114 | 1.235 / 158 | 1.235 / 158 | 1.026 / 114 |

Alle fire når 1.800 ticks med dropped0/failed0/SQfailed0/return0 og 40 released
biler. Gentagelser inden for hver variant har identisk terminal player-position
og race-state. Ændringen giver et andet forløb, som forventet ved fjernet bypass;
det ændrede arbejde er ikke en isoleret tidsmåling af proximity-funktionen.
GPU-vinduet er tidligt i racet; det er ikke en måling af samtlige aktive køreframes.
CPU og GPU overlapper og må stadig ikke lægges sammen.

Dette kontrollerede ligeud-forløb holder cirka 60 fps, men er ikke autopilot,
fysisk controllerkalibrering eller en gennemført bane: lap/finish er nul.
HDMI-kortet kunne fortsat ikke låses til capture. Næste milepæl er det naturlige
timeout/result/name-forløb uden indsprøjtet finish, derefter et faktisk course
completion-forløb og den fysiske styring.

[Validerede fire kørsler](../build/dc-native-proximity-wiring.3ZVRLb/summary.json).
`after-check.log`, `input-check.log`, buildlogs, input- og ELF-hashes ligger i
samme mappe. Ingen ny cache-/assetoptimering, gameplay-matematiktest eller CDI.

## Tidlig målgang afslører manglende racekonfiguration

Det planlagte naturlige timeout-forløb blev ikke godkendt: ved fuld gas og
centreret styring gik racet i mål efter én omgang, før timeren udløb. Den fælles
40-bils racestart lader spillerens checkpointfelter `total_laps`, `required` og
`visited` stå på nul efter scene-reset. Banens DNR7-header siger 8 omgange og
0 checkpoints. De underliggende grid- og rolling-resetfunktioner bevarer den
eksternt ejede konfiguration; det gamle single-car setup initialiserer den,
men 40-bils setup gør ikke. Derfor er den tidlige målgang ikke bevis for et
korrekt race. Næste afgrænsede rettelse er initialization ved fælles racestart,
ikke en ændring af checkpointalgoritmen eller fysikken.

Resultatforløbet og navneindtastningens egen timeout nåede normalt tilbage til
loaderen. Testens title-handoff-intercept signalerede enDjinn-shutdown; den
180 sekunders watchdog blev ikke udløst. Ingen finishbit, timer eller phase
blev indsprøjtet. Gasinputtet var syntetisk; fysisk styring er ikke dækket.

| Måling eller hændelse | Observeret | Afgrænsning |
|---|---:|---|
| Record ved phase25 | result2, notification1, eligible2, clock1682 | Uventet målgang, ikke naturligt timerudløb |
| Spillerens omgang / completed | 1 / 1 | Assetkravet er 8 omgange |
| Race ticks / resterende tid | 2632 / 1517 | Terminal phase29; må ikke kaldes timeout |
| Resultatets tavse commands / hash | 16 / dafdfc25 | Ingen lydafspilning |
| Navneforløbets ticks / committed | 1506 / 1 | Egen timeout, ingen A-inputinjektion |
| Hele practice-loopinterval gennemsnit / max ms | 16,686 / 19,936 | 3446 intervaller; inkluderer resultater, ikke efterfølgende name-mode |
| CPU-update gennemsnit ms | 4,209 | 2743 race/finish-samples; results/name returnerer uden denne måling |
| Rendercallbacks CPU gennemsnit ms | 6,727 | Race/finish-geometri; ikke samlet resultat/name-rendering |
| GPU gennemsnit / max ms | 6,060 / 9,194 | Kun de første 596 samples |
| Billedskift / vblanks efter 64 frames | 3381 / 3383 | To manglende billedskift i practice-vinduet |
| Billedskiftinterval max / over20ms | 33,306 ms / 2 | Ikke stabil én-flip-per-vblank gennem hele forløbet |
| Dropped simulationstid / failed / SQfailed | 0 / 0 / 0 | Program returnerede0; dette godkender ikke forkert målgang |

CPU og GPU overlapper. Tallene er ikke en ny hurtigere rendererbenchmark og
må ikke sammenlægges eller sammenlignes som ens workload med 1800-tick-kørslerne.
Resultaternes 674 ticks og actorens 449 steps er eksisterende kilde-/hostkontrol,
ikke nye observerede hardwarecounters ved handoff. Ingen blind gentagelse eller
produktionsrettelse i dette forsøg; loaderens readiness blev bekræftet bagefter.

[Hardwarelog](../build/dc-native-natural-timeout.8AgxaL/run-1.log),
[observerede felter](../build/dc-native-natural-timeout.8AgxaL/summary.json) og
[afvist timeoutaccept](../build/dc-native-natural-timeout.8AgxaL/acceptance.log).
`check.log`, `identity.sha256` og `assets.sha256` identificerer buildet og de
143 uændrede assets. Analyzer exit1 er det forventede afslag, ikke en buildfejl.
