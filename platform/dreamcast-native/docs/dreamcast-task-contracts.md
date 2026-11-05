# Afgrænsede opgaver til Daytona på Dreamcast

Arbejdsform og første opgavekø, 9. oktober 2026. Formålet er at dele arbejdet i små
leverancer, som en anden kan kontrollere. Teknisk ansvarlig ejer
arkitektur, prioritering, årsagsanalyse, integration og hardwaregodkendelse.
Listen er ikke en ordre om at implementere alle forslag eller starte alle opgaver.

## Revideret måltekst

Prioritet fra 11. oktober 2026: gameplay først. Start med den native cinematiske
attract-sekvens og tegn kun biler, selve vejbanen, kantsten og umiddelbare
bander/vægge, så kollisionsgrænserne er synlige. Skjul dekorative omgivelser,
træer, klipper, tribuner, skilte og himmel uden at fjerne simulation, modstanderlogik,
kollisioner eller kamerahændelser. Mål stabil 60 fps på hardware, inklusive
langsomme frames, før yderligere renderer-mikrooptimering. Bevar simulationens
eksisterende faste tidsbase; 60 fps må ikke opnås ved at ændre spillets hastighed.
Fortsæt derefter mod et korrekt, inputstyret fuldt race med samme reducerede
visning. Et 30-sekunders attract-forløb beviser ikke et korrekt fuldt race.

Når gameplay og den reducerede visning er accepteret, udformes den komplette
renderer på Dreamcastens præmisser med offline-forberedte assets. Genbrug
verificerede enDjinn-, SH4ZAM- og SQ-dele; en ny renderer er ikke en ordre om at
kassere alle fungerende hjælpefunktioner. Kravet om visuel ækvivalens nedenfor
gælder de bevarede assets og den senere komplette renderer; den midlertidige
udeladelse af dekorationer er et bevidst valg.

Fortsæt den Dreamcast-native Daytona-version i `platform/dreamcast-native/` frem mod et
sammenhængende, spilbart raceforløb og dokumenteret framearbejde under 16 ms i
repræsentative hardwaretests. Brug PC-repoet som adfærdsreference, ikke som en
board-simulator der skal flyttes til Dreamcast. Bevar PC-adfærden. enDjinn ejer
initialisering, input, gameplay-loop, renderkøer og shutdown. Brug SH4ZAM først
til matematiske primitiver og egnede kopieringsoperationer, følg bibliotekets
best practice, og hold gameplay og rendering tydeligt adskilt. Forbered assets
offline i Dreamcast-egnede formater og størrelser inden for RAM/VRAM-budgetterne;
bevar direkte SQ-submission, resident teksturstrategi, 24bpp uden dithering og
tavs lyd indtil AICAFlow. Verificér relevante milepæle på hardware, når den er
tilgængelig og frigivet; fortsæt nyttigt host-arbejde uden at kalde det hardwarebevis.

Assetoptimering skal bevare det synlige resultat, også tekstur-, UV- og
lysadfærd. Saml overopdelte flader og lav strips offline, hvor det sparer arbejde;
quads er ikke et krav. Vælg trekant, fire-vertex strip eller længere strip efter
den enkleste effektive løsning, ikke efter en bestemt primitivtype.

Asset-transformeren skal kunne genbruges på alle tre baner. Genbrug den
eksisterende offline-packer og tillad validerede særregler i en JSON-sidecar
pr. oprindeligt geometri-ID, ikke hardkodede mesh-numre fra begynderbanen.
Særregler er ikke tilladelse til at bryde visuel ækvivalens eller RAM/VRAM-budgetter.

Arbejd som teknisk ansvarlig og orkestrator efter kontrakterne i denne fil.
Behold arkitektur, prioritering, årsagsanalyse og endelig godkendelse centralt.
Vedligehold en prioriteret opgavekø med afhængigheder, og adskil originalviden,
aktuel implementering og antagelser. Del små mekaniske undersøgelser og kendte, afgrænsede implementeringer ud
som separate opgaver.
Hver opgave skal have præcise forudsætninger, skriveområde, afgrænsning,
reproducerbar acceptkontrol, stopregel og evidensleverance. En påvist fejl eller
manglende forudsætning er en gyldig leverance uden en rettelse.

Undersøg original adfærd og instrumentering, find første semantiske afvigelse,
afprøv én isoleret hypotese, og ret det fælles årsagssted med en relevant
regressionskontrol. Kontrollér instrumenteringens påvirkning; svæk aldrig tests
eller flyt forventninger for at få PASS. Nye gameplay-matematiktests er
fravalgt. Reviewer eftergår diff og genkører afgørende kontroller
med registrerede inputs, buildflags, hashes og exitkoder. Integrér accepterede
ændringer i små lokale commits, når de kan isoleres fra andet igangværende arbejde.
Parallelisér kun uafhængige opgaver; serialisér fælles filer og hardware.
Genbrug værktøjer, undgå generelle frameworks og blinde gentagelser. Målet er
først opfyldt, når spilforløb og performance er dokumenteret, ikke når
opgavekortene er afsluttet.

Denne tekst og arbejdsformen er projektets styrende, opdaterede instruks;
`platform/dreamcast-native/HANDOFF.md` henviser hertil. Målet er ikke erklæret fuldført.

## Hvad vi ved og hvad vi mangler

| Emne | Originalen | Aktuel native kode | Ikke bevist endnu |
| --- | --- | --- | --- |
| Bilernes LOD | Importen angiver kildeområderne 8590..85f4 og 836c..847c | `include/intro.h` vælger niveau 1–4 og kvoter; `car_detail.h` beregner synlighed/dybde; scene-rendereren vælger modeller | Hvor meget hvert niveau koster i det aktuelle hardwareforløb |
| Quads og strips | Kildens fan-diagonal og winding skal bevares | D33 opt-in DNM5-kæder bevarer XYZ/UV og rækkefølge; SQ kræver samme aktuelle binding og intakte vertices, ellers face-fallback | Fuld bane og dybde-/clipping-ækvivalens; enkelt hardwarepar18,011/17,991ms viser ingen gevinst |
| Statiske omgivelser | Lysparametre og normals kommer fra importerede data | Udvalgte ensidige faces får luma ved attach; andre beholder fallback | At alle omgivelser kan bruge denne vej |
| Normal cones | Native optimering, ikke en påstand om originalens arkitektur | D15 bruger faktiske FTRV-transformer; target-verifier består 300 ticks; D03 isolerer live-tællere | Alle kameraforløb; usikre grupper bruger stadig per-face CPU-facing |
| Frametid | Ingen ny originalmåling i denne opdeling | To quiet1800tick-runs den 9. oktober: 17,983/18,007 ms; GPU 8,870/8,875 ms i samplevinduet | Fuld bane med aktiv kørsel, under16 ms og isoleret optimeringsgevinst |

Stier i tabellen er under `platform/dreamcast-native/`. Originalhenvisningerne er
kodekommentarernes provenance, ikke en ny uafhængig originalverifikation.
Gamle bring-up-noter i `HANDOFF.md` er historik; kildekode og konkret evidens
skal holdes adskilt fra dem.

## Fælles kontrakt

Læs relevante instruktioner og hele den berørte funktion samt dens kaldere.
Genbrug eksisterende værktøjer. Ingen nye frameworks, afhængigheder eller
arkitekturlag til denne opgavekø.

- Lille opgave ("billig"): mekanisk audit, optælling, dokumentation eller en helt
  fast ændring.
- Mellemstor opgave: afgrænset implementering med kendt kontrakt. Vurdér omfanget
  efter konkret usikkerhed, ikke blot filens længde.
- Teknisk ansvarlig: uklare ejerskaber, numerisk sikkerhed, timing og review.
- Alle worker-opgaver er uden consoleadgang. Kun teknisk ansvarlig uploader,
  genstarter, ændrer cache/PVR-tilstand eller gennemfører hardwareforsøg.
- Ingen ændringer i gameplay-matematik eller nye tests af den i denne kø.
  Rendering-/pack-/buildchecks er særskilte. SH4ZAM skal ikke genimplementeres.
- Bevar enDjinns loop/input/køer, direkte SQ-submission, 24bpp og tavs lyd.
  Ingen runtime-VQ, nye runtime-textureuploads, PAL8 eller svækkede budgetter.
- Ingen ændringer i PC-reference, ROM-data eller søsterprojekterne enDjinn og
  SH4ZAM. ROM-afledte rapporter/billeder bliver i ignorerede lokale mapper.
- En opgave ændrer kun kontraktens skriveområde. Overlap betyder serialisering,
  også for testfiler. Højst to uafhængige worker-opgaver ad gangen i første runde.

Workerens leverance skal indeholde: opgave-ID, diff eller fund med fil/funktion,
præcis kommando og arbejdsmappe, inputstier og SHA-256, relevante buildflags og
compileridentitet, exitkode, evidensstier og resterende usikkerhed. Ved ingen
kodeændring angives det eksplicit. En manglende forudsætning er et resultat,
ikke tilladelse til at vælge en anden assetpakke eller rette noget andet.

Reviewer læser diffen og genkører den afgørende kontrol. Eksisterende test skal
fortsat bestå; den nye kontrol skal kunne afvise den konkret forkerte variant.
Forventede værdier, tolerancer og coverage må ikke flyttes for at få PASS.
Instrumentering kontrolleres mod den uinstrumenterede outputsti.

Accepterede implementeringer integreres som små lokale commits, men kun når
opgavens ændringer kan isoleres fra andet igangværende arbejde. Ved denne opdeling er
hele `platform/dreamcast-native/` untracked, og mange andre filer er ændrede. Ingen
`git add .`, nulstilling eller automatisk indlemmelse af dette forarbejde.
En ren worktree fra HEAD indeholder derfor ikke nødvendigvis det aktuelle spil.
Registrer en filbaseline før hver worker; afklar snapshot/staging før commits.

## Afhængigheder og startorden

| ID | Leverance | Omfang | Klarhed og rækkefølge |
| --- | --- | --- | --- |
| D01 | Korrekt DNM4-hjælpetekst | Billig | Klar; lille kalibreringsopgave |
| D02 | Bitpræcis validering af reserveret DNM4-felt | Mellem | Klar; efter D01 hvis begge redigerer packer/tests |
| D03 | Sessionafgrænsede normal-cone-tællere | Mellem | Klar; eksklusiv adgang til renderer |
| D04 | Kontrol af profiltællernes triangle-enhed | Mellem | Efter D03, samme SQ-testfil |
| D05 | Evidens for genkompilering med gemt assembler | Billig | Kræver ansvarligs fastlåste buildkommando |
| D06 | Registrér eksisterende component-check | Billig | Klar; må ikke overlappe D05-build |
| D07 | Materialekontaktark til håndoptimering | Billig | Kræver friske validators; ingen kodeændring |
| D08 | Strip-potentiale for den valgte world-pakke | Billig | Samme låste inputs som D07 |
| D09 | Præcis oversigt over bilernes LOD-vej | Billig | Klar; læsning, ikke LOD-ændring |
| D10 | Inventar over seks render-flags | Billig | Klar; ingen flag fjernes endnu |
| D11 | Reachability og levetid for face-binding-cachen | Mellem | Læsning; før hardwaregodkendelse |
| D12 | Konservativitet af model-space normal cones | Ansvarlig | Beslutningsopgave, ikke billig implementering |
| D13 | SH4-instruktionsaudit af tre renderkerner | Billig + review | Efter D05 og fastlåst frisk build |
| D14 | Matchet hardwarekontrol og A/B | Ansvarlig | Efter D03, D04, D11 og D12 |
| D15 | Ret den påviste normal-cone-afvigelse | Ansvarlig | D12 modcase; før performance A/B |
| D16 | Sammenlign gemte normals med triangle-winding | Mellem + review | Hostfund accepteret; ingen global PVR-ændring |
| D17 | Brug face-binding-cache efter per-face-culling | Ansvarlig + afgrænset testworker | Afvist efter A/B/B/A: cirka0,49ms langsommere; produktionsændring tilbageført |
| D18 | Find første manglende race/resultatovergang | Billig + review | Fase27 for podieplaceringer mangler; timeout/resultatvej findes |

Start med D01 og den skrivebeskyttede D09. Review leverancerne, før flere
implementeringer sættes i gang. D07/D08 genbruger værktøjer; D12 kan afvise en
optimering. Der er ingen gevinst ved at implementere alle kort for deres egen skyld.

### Review af første runde 9 oktober 2026

- **D01 accepteret:** Én hjælpelinje rettet, `--help` genkørt med exitkode 0.
  Reviewer genskabte gammel tekst i en læsestrøm og fik nøjagtigt workerens
  baselinehash; ingen algoritmeændring. Ingen commit: filen indeholder også
  eksisterende untracked arbejde, som ikke må indlemmes automatisk.
- **D09 accepteret med afgrænsning:** Kode og det katalog, som depfiles for
  `dc-native-race-unconditional-check` angiver, er eftergået. Level 1–4 svarer
  før kvotenedsættelse til Z-intervallerne `(100,256]`, `(35,100]`, `(20,35]`
  og `[0,20]`, forudsat cell/wedge-synlighed. Katalogets spiller-række 0 vælger
  body-binding 184 på alle fire niveauer; modstander-række 1 vælger 5/4/3/2.
  Level 3/4 tilføjer separate hjul/vinduer; level 2–4 kan få skygge. Lavere
  crash-levels kan få en attachment; cinematic special-actor har egen vej.
  Inspector bruger egne celler, orden og levels. Det er ikke nye polygon-
  optællinger eller bevis for, hvilken ELF der aktuelt er på konsollen.
- **D12 fandt en modcase:** Kameraet `diag(1.0005,1,1)` accepteres af den aktuelle
  ortogonalitetskontrol. Med normal `(1,-1,0)`, punkt `(1000,1000.5,10)`,
  normalradius 0 og punktradius 0,01 giver den faktiske model-space-funktion
  `-1` (forkast), mens det transformerede medlems dot-produkt er `+0.500183`.
  Model-space-dot er `-0.5`. Reproduceren bruger de aktuelle appfunktioner og
  SH4ZAMs software-backend, med ASan/UBSan; exitkode 0 betyder **modcasen er
  reproduceret**, ikke at culling er godkendt. Det er ikke påvist, at en normal
  spilkameramatrix når dette tilfælde, og der er ingen ny SH4-måling.

D12-evidens og genkørsel:
`python3 build/dc-native-cone-contract.Fsnhqp/check_cone_contract.py`.
Inputs, compiler og output er gemt i samme mappes `evidence.txt`.
Ingen rendering er rettet i denne runde. D14 afventer en særskilt rettelse/review
af den manglende konservative fejlgrænse; en mulig sikker baseline er den
eksisterende FTRV-formulering. Udvid ikke tolerancerne for at skjule modcasen.

## Kontrolkommandoer

Seneste review: **D03, D04 og D11 accepteret med deres afgrænsninger**.
D03 nulstiller fire tællere ved første live-frame og release; reviewer genkørte
kontrollen af faktisk resetkode. D04 tæller fem inputcases korrekt og afgiver
identiske 756 bytes med/uden profilering ved begge skalaer. D11 beviser cache-
opslagets lighed og levetid i de aktuelle kaldere, ikke numerisk lysækvivalens.
Detaljer: `build/dc-native-quiet-profile.2KzpqN/evidence.txt`.

Tre hardwarekørsler sluttede fejlfrit: quiet17,983 ms, profile25,662 ms,
quiet18,007 ms. Alle når samme1800tick racehændelse, men tegner forskelligt antal
frames; dette isolerer ikke timeromkostning eller en renderingoptimering.
**D14 er ikke fuldt accepteret:** anden profilkørsel venter under indlæsning;
ingen reset eller gentaget upload. Se HANDOFF før hardware.

Senere samme dag er **D02 og D15 accepteret inden for deres testscope**.
D02's nye signed-zero-case fejler mod den gamle reader og består mod den nye;
reviewer har genbygget/rerunnet sanitizers og kontrolleret den faktiske world-pakke.
D15's faktiske target-funktioner består 100450 samplede medlemstests per host-
compilerkonfiguration og fejler mod før-headers på D12-modcasen. Den endelige
Dreamcast-verifier har kørt 300 ticks uden fejl og returneret til loaderen.
De 28,513 ms er med verifier, ikke en ren performancekørsel. Evidens og endelig
ELF-identitet: `build/dc-native-cone-fix.2WaB51/evidence.txt`.

Den historiske D12-probe ovenfor læser aktuelle kilder og reproducerer derfor
ikke længere den rettede fejl. Brug den nye regression mod de gemte før-headers:

```sh
python3 platform/dreamcast-native/tests/test_normal_cones.py \
  build/dc-native-cone-fix.2WaB51/scenery_camera.before.h \
  build/dc-native-cone-fix.2WaB51/model_submission.before.h
# Forventet exitkode 1: gammel cone=-1, faktisk medlemsdot positiv.
```

Alle kommandoer køres fra repository-roden. Brug `mktemp -d` under `build/`
til nye evidensmapper; overskriv ikke en tidligere test. Ansvarlig bygger og
fastlåser aktuelle validators, før assetopgaver udleveres:

```sh
cmake --build build --target check_native_meshpack check_native_material_pack -j4
python3 platform/dreamcast-native/tests/test_prepare_models.py build/check_native_meshpack
python3 platform/dreamcast-native/tools/inspect_quad_strips.py --self-test
python3 platform/dreamcast-native/tools/inspect_scenery_components.py --self-test
python3 platform/dreamcast-native/tests/test_sq_quads.py
```

På oprettelsesdagen er de sidste tre kommandoer kørt med exitkode 0.
SQ-testen afprøver begge X-skalaer og ASan/UBSan; dens forventede
`FAIL SQ TA budget` er en negativ test. Det er ikke en hardwaretest.
Validators er lokaliseret, men ikke genbygget som led i denne opgaveliste.

Nedenstående lokale mount findes ved oprettelsen:

```sh
task_assets="$PWD/build/dc-native-normal-cone-runtime.jda1f8/assets"
```

Det er et eksplicit input, ikke en søgning efter den nyeste mappe. Ved en senere
kørsel skal eksistens, pack-validering og hashes kontrolleres igen.

## D01 Korrekt format i packerens hjælp

- **Formål:** Ret kun hjælpen for `--face-group-size`, som siger DNM3, selv om
  `group_faces` skriver DNM4 med spatial- og normalgrupper.
- **Forudsætninger:** `tools/prepare_models.py`, `group_faces`, magic
  `0x344d4e44`; eksisterende `test_prepare_models.py` kontrollerer denne magic.
- **Skriveområde:** `platform/dreamcast-native/tools/prepare_models.py`, kun hjælpeteksten.
- **Uden for scope:** Ingen format-, parser-, grupperings- eller byteændringer.
- **Accept:** `python3 platform/dreamcast-native/tools/prepare_models.py --help` viser
  DNM4 og normalgrupper for flaget; diffen indeholder kun teksten.
- **Stopregel:** Returnér fund, hvis formatet i den aktuelle revision er anderledes.
- **Leverance:** Diff, kommando/exitkode og den relevante hjælpelinje.

## D02 Reserveret DNM4 felt skal være nul i alle bits

- **Formål:** Valider de fire serialized-zero bytes i `normal_length` bitpræcist.
  Float-testen `!=0` accepterer negativt nul; feltet er runtime-cache, ikke input.
- **Forudsætninger:** `code/meshpack.c:validate`, `include/meshpack.h`,
  `tools/prepare_models.py:group_faces` og eksisterende malformed-pack-fixture.
- **Skriveområde:** `platform/dreamcast-native/code/meshpack.c` og
  `platform/dreamcast-native/tests/test_prepare_models.py`.
- **Uden for scope:** Ingen recordstørrelse, filversion, lysberegning eller
  type-punning med aliasingbrud. Ingen ændring af valide packbytes.
- **Accept:** Genbyg validator og kør pack-testen ovenfor. Tilføj tilfælde med
  `0x80000000`, `0x00000001`, `0x3f800000` og `0x7fc00000`: alle afvises;
  `0x00000000` accepteres. Negativt nul skal afsløre den gamle validering.
- **Stopregel:** Hvis eksisterende autoriserede eksportere bruger feltet til data,
  returnér det input og kaldestedet; opfind ikke en migration.
- **Leverance:** Diff, validatoridentitet, testresultat og byte-offset beregnet
  ud fra layoutet, ikke gættet fra én virkelig pakke.

## D03 Normal cone statistik skal starte forfra

- **Formål:** De fire `draw_normal_*`-tællere skal beskrive den aktuelle live
  rendersession, uden startup-oracles eller tidligere title/race-sessioner.
- **Forudsætninger:** Alle læsninger/skrivninger i `model_renderer.c` og
  `model_submission.h`; `nm_render_enqueue_objects` nulstiller allerede andre
  profilfelter før første live frame, og `nm_render_release` rydder ressourcer.
- **Skriveområde:** `platform/dreamcast-native/code/render/models/model_renderer.c` og
  `platform/dreamcast-native/tests/test_sq_quads.py`.
- **Uden for scope:** Ingen culling-, tæller-enheds-, render-, timer- eller
  levetidsændringer ud over de fire statistikfelters nulstilling.
- **Accept:** SQ-testen består; en lille kontrol af faktisk resetkode seed'er
  tællerne med forskellige værdier, kontrollerer første-frame-reset og en ny
  session efter release. En manglende reset af ét felt skal give fejl.
- **Stopregel:** Kræver host-fixturen en bred renderer-ombygning, returnér den
  konkrete hindring og foreslå den mindste kontrol; udvid ikke arkitekturen.
- **Leverance:** Diff, reproducerbar kontrol og definition af tællernes interval.

## D04 Test profiltællernes triangle enhed

- **Formål:** Bevis at `draw_clip_paths` tæller inputtrekanter, ikke vertexer,
  strips eller outputtrekanter, også når to trekanter sendes som én quadstrip.
- **Forudsætninger:** `sq_emit_range` og eksisterende host-SQ-fixture. Den
  aktuelle fixture bygger ikke med `NATIVE_PROFILE`; det hul skal dækkes.
- **Skriveområde:** `platform/dreamcast-native/tests/test_sq_quads.py` alene.
- **Uden for scope:** Ingen produktionsrettelse, nye timere eller fake
  performance-tal. Brug rigtig emitterkode, ikke en kopi af tællealgoritmen.
- **Accept:** `python3 platform/dreamcast-native/tests/test_sq_quads.py` kontrollerer
  intakt quad=2 inputtrekanter, triangle=1 samt helt afvist og nærplansklippet
  input. Kontroller særskilt overlappende xy-/near-touch-felter. Profileret og
  uprofileret fixture skal afgive identiske headers/vertexer for samme input.
- **Stopregel:** Ved mismatch leveres første afvigende case; ret ikke emitteren
  eller forventningen i denne opgave.
- **Leverance:** Testdiff, forventede/observerede tællere og outputlighed.

## D05 Gemte assemblerfiler må ikke erstatte C genkompilering

- **Formål:** Undersøg om Makefiles eksplicitte C-regel stadig vælges, når der
  findes `.s` fra `-save-temps=obj`, og et inkluderet header ændres.
- **Forudsætninger:** Ansvarlig leverer én komplet fungerende buildkommando med
  fastlåste assetstier og isoleret `ENJ_BUILDDIR`/`ENJ_BINDIR`; ingen defaults
  eller historiske kommandoer gættes. Læs Makefile og enDjinns regel read-only.
- **Skriveområde:** Ingen kildefiler; kun ny ignoreret evidensmappe.
- **Uden for scope:** Ingen `touch` på eksisterende headers, intet clean, ingen
  ændring af enDjinn eller compilerflags.
- **Accept:** Gem første buildkommando/log og derefter samme kommandos dry-run
  med `-n -W include/render/models/model_submission.h`. Vis den valgte
  `model_renderer.c -> .o`-kommando trods eksisterende `.s`; kontrollér også
  geometry-reglen. Header-afhængigheden skal være til stede i den genererede `.d`.
- **Stopregel:** Manglende inputs eller afhængighed, assembler som input eller
  ingen rebuild: returnér loggen, ikke en buildsystemrettelse.
- **Leverance:** Kommandoer, source/header/.s/.o-hashes og første forkerte regel,
  hvis der er en. PASS gælder regelvalget, ikke spillets korrekthed.

## D06 Kør den eksisterende component kontrol i check

- **Formål:** Medtag `inspect_scenery_components.py --self-test` i eksisterende
  `make check`, ved siden af strip-værktøjets allerede registrerede self-test.
- **Forudsætninger:** Værktøjets eksisterende test af delte kanter, isolerede
  komponenter, punktkontakt og degenererede kanter. Ingen ny testalgoritme.
- **Skriveområde:** `platform/dreamcast-native/Makefile` alene.
- **Uden for scope:** Ingen generel test-discovery, gameplaytests eller omlægning
  af CMake/check-targets.
- **Accept:** Kommandoen består direkte; den fastlåste native `make check`-log
  viser, at den køres. Make må ikke ignorere dens exitkode.
- **Stopregel:** Hvis kontrollen allerede er registreret, returnér no-op-evidens.
  Manglende native buildinputs betyder kun delvist verificeret levering.
- **Leverance:** Énlinjediff og direkte/aggregat kontrolstatus hver for sig.

## D07 Materialekontaktark til håndoptimering

- **Formål:** Lav et navngivet kontaktark for den aktuelle world-pakke, så man
  kan identificere vej, klipper og tribuner via materialenumre.
- **Forudsætninger:** Fastlåst `task_assets`, friske mesh/material-validators og
  installeret Pillow. Brug `inspect_scenery_materials.py` uændret.
- **Skriveområde:** Kun en ny ignoreret mappe under `build/`.
- **Uden for scope:** Ingen automatisk semantisk klassifikation, geometri-
  fjernelse, mipmap/VQ-encoding eller ændring af assetpakker.
- **Accept:** Kør `python3 platform/dreamcast-native/tools/inspect_scenery_materials.py
  build/check_native_meshpack build/check_native_material_pack
  "$task_assets/world.dnm" "$task_assets/world.dnt" "$task_output"` med en
  endnu ikke eksisterende output-undermappe. Summen af `faces` i JSON skal
  matche packheaderen; reviewer åbner PNG'erne og kontrollerer læsbare IDs.
- **Stopregel:** Manglende binding eller forkert packtype: returnér præcis fejl;
  vælg ikke en vilkårlig variant som erstatning.
- **Leverance:** Hashes, log, PNG/JSON-stier; noter at arket viser lagrede
  lysankre, ikke nødvendigvis den endelige modulerede farve.

## D08 Strip potentiale uden at ændre geometri

- **Formål:** Find de mest lovende meshes i præcis den world-pakke, vi bruger,
  og adskil materialestabile muligheder fra optimistiske øvre estimater.
- **Forudsætninger:** Samme validators/pakker som D07; eksisterende
  `inspect_quad_strips.py` har allerede kædeanalyse og self-test.
- **Skriveområde:** Kun ny ignoreret evidensmappe; ingen sourceændring.
- **Uden for scope:** Ingen ny stripifier, welding, face-reordering eller
  slutning fra sparede bytes til sparede millisekunder.
- **Accept:** Kør `python3 platform/dreamcast-native/tools/inspect_quad_strips.py
  build/check_native_meshpack "$task_assets/world.dnm" --materials
  "$task_assets/world.dnt" --material-reader build/check_native_material_pack`.
  Rapportér eksisterende JSON-felter for source-order/reordered og stable/
  optimistic hver for sig; tre konkrete mesh-ID'er skal kunne genfindes.
- **Stopregel:** Hvis analysen ikke identificerer en sikker kandidat, er dét
  leverancen. Manglende lys-/clippingbevis er ikke implementeringstilladelse.
- **Leverance:** Kommando, hashes, uændret JSON-output og højst tre kandidater
  med den præcise resterende hindring for runtime-brug.

## D09 Bilernes eksisterende LOD vej

- **Formål:** Besvar præcist, hvilke LOD-niveauer spilleren og modstanderne kan
  få, hvilke body-/hjul-/vindues-/shadowdele der vælges, og hvor beslutningen tages.
- **Forudsætninger:** `include/car_detail.h`, `include/intro.h`,
  `code/render/scene/practice.c`, deres kaldere og det valgte genererede bilkatalog.
- **Skriveområde:** Ingen kildefiler; rapport i leverancen.
- **Uden for scope:** Ingen ændrede afstande, kvoter, gameplaymatematik eller
  påstand om modellernes polygonantal uden faktisk optælling.
- **Accept:** Lever en tabel for level 0–4 med grænser, kvotenedsættelse,
  modelopslag og renderdele, hver med fil/funktion. Følg almindelig race og
  pause-inspector særskilt. Reviewer kan eftergå hvert felt med `rg` og kildekode.
- **Stopregel:** Mangler det genererede katalog, oplys hvilke modelidentiteter
  der er ukendte; gæt ikke ud fra `6-level` eller filnavne.
- **Leverance:** Tabellen, kaldestier og forskellen mellem originalhenvisning,
  aktuel implementering og umålt omkostning.

## D10 Seks render flags og deres reelle brug

- **Formål:** Kortlæg kun `NATIVE_DIRECT_SQ`, `NATIVE_RESIDENT_CAR`,
  `NATIVE_BILINEAR`, `NATIVE_CAMERA`, `NATIVE_PROFILE` og `NATIVE_BOUNDS_VERIFY`.
- **Forudsætninger:** `local.cfg.mk`, `Makefile`, alle matches i native code,
  include og tests; ønsket om færre produkt-ifdefs.
- **Skriveområde:** Ingen kildefiler; rapport i leverancen.
- **Uden for scope:** Ingen fjernelse, global definition eller omdøbning af flags.
  Diagnostik er ikke automatisk død kode.
- **Accept:** Reproducerbar `rg -n`-kommando og seks rækker med definition,
  standardværdi, produkt-/diagnostikbrug, inkompatibiliteter og berørte tests.
  Rapportér ukendte konfigurationer i stedet for at antage, at de virker.
- **Stopregel:** Hvis et flag ændrer ressource-/PVR-ejerskab, markér det som
  arkitekturbeslutning; løs det ikke under denne audit.
- **Leverance:** Inventaret; ansvarlig vælger højst ét efterfølgende cleanup-kort.

## D11 Face binding cachens betingelser og levetid

- **Formål:** Undersøg at enhver læsning af `world_draw.face_bindings` rammer en
  initialiseret entry med samme binding som fallback-opslaget ville vælge.
- **Forudsætninger:** `nm_render_world_attach`, `sq_prepare_batch`,
  `nm_render_release`, `sq_world_static_luma` samt kaldere/failure-cleanup.
- **Skriveområde:** Ingen kildefiler; rapport i leverancen.
- **Uden for scope:** Ingen rettelse, toleranceændring, ny cache eller fjernelse
  af fejlchecks. Et testmæssigt host-bypass tæller ikke som target-coverage.
- **Accept:** En tabel over source/world, front/uncertain/rear, nul/positiv
  cached luma, sentinel og attach-fejl viser writer, reader, indeks og fallback.
  Citer præcis host-testdækning og de target-grene den ikke udfører.
- **Stopregel:** Første ubeviste betingelse eller modstridende sti afslutter
  undersøgelsen med inputbetingelser og relevante kaldere.
- **Leverance:** Beviskæde eller konkret modcase; ingen generel 'ser fint ud'.

## D12 Normal cone beslutningen beholdes centralt

- **Formål:** Afgør om model-space-testen er konservativ for alle matricer,
  som `nm_scenery_normal_group_view` accepterer.
- **Forudsætninger:** `scenery_camera.h`, `sq_normal_group_facing`, DNM4 cone-
  kontrakten og den eksisterende FTRV-verifier. Kameratesten tillader afvigelser
  op til 0,002 fra ortogonalitet; den algebraiske identitet forudsætter en rigid
  matrix. Dette er et reviewspørgsmål, ikke en allerede bevist fejl.
- **Skriveområde:** Ingen implementering i dette kort; beslutning/evidens hos
  teknisk ansvarlig. En eventuel rettelse får sit eget snævre kort.
- **Uden for scope:** Ingen ny SH4ZAM-testkampagne, gameplayændring eller
  forøgelse af tolerancer for at få verifieren til at bestå.
- **Accept:** Dokumentér håndteringen af residualet
  `nᵀ(RᵀR−I)p` og cone-radius-/afrundingsbidrag, eller påvis en konkret modcase.
  Test en faktisk implementeringssti; Python-identiteten for tre eksakte
  rotationer alene kan ikke godkende target-grenen.
- **Stopregel:** Manglende sikker grænse betyder 'ikke godkendt til speedup-
  konklusion'; foreslå konservativ fallback frem for at gætte.
- **Leverance:** Beslutning og én reproducerbar renderingskontrol; fysisk
  SH4-verifikation forbliver særskilt D14.

## D13 Instruktionsaudit af tre renderkerner

- **Formål:** Find konkrete dyre kald, float/double-skift, XMTRX-loads/stores
  og SQ-stores i `sq_prepare_batch`, `sq_emit_range` og `sq_normal_group_facing`.
- **Forudsætninger:** D05, frisk handopt-build og samme sources/flags; læs de
  anvendte SH4ZAM-primitivers lokale dokumentation og eksempler først.
- **Skriveområde:** Ingen kildefiler; kun ignoreret assembler-/rapportoutput.
- **Uden for scope:** Ingen assemblerpatch, blanket-udskiftning af matematik,
  funktionstælling som cykeltal eller slutning fra `pref` alene til data-prefetch.
- **Accept:** For hvert fund leveres funktion/inlined kaldested og et lille
  assemblerudsnit fra compilerens `.s`, koblet til source og buildhash. Skeln
  mellem SQ-flush og prefetch, samt mellem lovligt XMTRX-bankarbejde og double-
  mode. Tom eller forkert symbol-disassembly er ikke PASS.
- **Stopregel:** Kan optimeret/inlined kode ikke kobles sikkert til et kaldested,
  angiv det; ændr ikke compileflags for at få et mere bekvemt svar.
- **Leverance:** Højst fem konkrete fund med evidens, ikke en generel SH4-guide.

## D14 Matchet hardwarekontrol før optimeringskonklusion

- **Formål:** Kontrollér seneste renderændringer og mål dem mod et kendt build
  med samme semantiske raceforløb og assetidentitet.
- **Forudsætninger:** D03/D04/D11/D12 reviewet; ansvarlig har consoleejerskab,
  verifier og baseline/candidate med isolerede buildmapper. Hvis en gammel
  kildebaseline ikke findes, kan en gammel ELF ikke påstås isolere én ændring.
- **Skriveområde:** Kun nye lokale logs/build-/captureartefakter. Eventuel
  kildekandidat skal allerede være særskilt reviewet.
- **Uden for scope:** Ingen worker-upload, parallel kos-tool, automatisk reset-
  løkke, ny draw-distance, skjulte geometriudeladelser eller debugfarver som timing.
- **Accept:** Kør verifier først; derefter mindst to matchede baseline/candidate-
  par med oracles/logging ude af målevinduet. Registrér ticks/semantisk fase,
  antal frames, whole-loop, CPU-faser, GPU, waits, TA og uploads, hvor de faktisk
  er målt. CPU og GPU overlapper og må ikke lægges sammen. Begge builds skal
  returnere til loaderen; gem exitkode og konfiguration. Ingen nye gameplay-
  matematiktests. Instrumenteringens synlige output skal være kontrolleret.
- **Stopregel:** Verifierfejl, frysning, forskelligt workload eller manglende
  loaderkontakt: stop og gem første afvigelse, ikke gentagne blinde forsøg.
- **Leverance:** Afgrænset korrekthedsresultat og måletabel. En negativ eller
  uklar performanceforskel er et fuldt gyldigt resultat; ingen <16 ms-påstand
  uden tilsvarende måling.

## D15 Ret den påviste normal cone afvigelse

- **Formål:** Ret D12-modcasen ved fælles facing-beregning uden at fjerne
  gruppernes front/rear-optimering eller ændre gameplay.
- **Forudsætninger:** Gemte før-headers i `build/dc-native-cone-fix.2WaB51/`;
  den tidligere fejlagtige algebra erstattes med `(Rn).(Rp+t)` via SH4ZAM.
  XMTRX er allerede indlæst i `sq_prepare_batch`. Scenens near-rigid gate
  bevares; bounden skal inkludere dens tilladte stræk samt afrunding før
  cancellation, inklusive normal-/punktradier.
- **Skriveområde:** `include/render/models/scenery_camera.h`,
  `include/render/models/model_submission.h`, `tests/test_normal_cones.py`
  og kaldet til denne kontrol i `tests/test_sq_quads.py`, alle under native-roden.
- **Uden for scope:** Ingen nyt matrixload per gruppe, libraryændring, asset-
  ændring, gameplaymatematik eller ændrede per-face-verifierforventninger.
  Den gamle identity-only hostcheck erstattes af faktisk target-kode på
  SH4ZAMs softwarebackend; det er ikke en ny test af bibliotekets algoritmer.
- **Accept:** Ny kontrol skal fejle mod før-headers på D12-modcasen og bestå
  på ny kode. Afprøv begge facing-retninger, roterede/næsten-ortogonale views,
  radier, translation-cancellation og ugyldigt/overflow-input. Bevar virksomme
  front/rear-resultater, ikke altid uncertain. Normal native `make check` og
  et 300-ticks target-build med `NATIVE_BOUNDS_VERIFY=1` skal bestå og returnere
  til loaderen. Per-face rear/front-kontroller i `sq_prepare_batch` bevares.
- **Stopregel:** En forkert klassifikation eller hardware-verifierfejl stopper
  integrationen; ukorrekte input må ikke blive til en større tolerance.
- **Leverance:** Diff, mutationstest, host-/build-/hardwarelogs og endelig ELF-
  hash. Korrekthedskørslens tid er ikke en uinstrumenteret speedup-måling.

## D43 Genindlæsning mellem attract og race

- **Formål:** Ret CPU-meshpakkernes ejerskab ved sceneovergang, så START fra
  attract ikke forsøger at indlæse i en allerede indlæst `nm_pack_t`.
- **Forudsætninger:** Den reducerede attract er målt til cirka 60 fps i
  `build/dc-native-track-attract.sasfYW/`. `nm_practice_render_release` frigiver
  rendererressourcer, men beholder CPU-model/world; `nm_pack_load` afviser
  `pack->data != NULL`. Læs alle load/release-kaldere før en rettelse.
- **Skriveområde:** `code/render/scene/practice_assets.c` og én lille test under
  `tests/`; præcis normalisering i `test_practice_render_split.py` må følge med,
  men eksisterende kontrol af uændret matematik må ikke svækkes.
- **Uden for scope:** Ingen ændringer i gameplay-matematik, PVR-lifecycle,
  PC-reference, rendereroptimering, ROM eller teksturformater.
- **Accept:** Gentag load/release/load med de faktiske ejerskabsfunktioner og
  verificér korrekt enkelt frigivelse, delvis loadfejl og genindlæsning uden
  dangling borrowed pointers. Normal native `make check` skal bestå. Root
  verificerer derefter sceneovergangen på hardware; worker har ikke consoleadgang.
- **Stopregel:** Uklart GPU-/CPU-ejerskab eller behov for ny PVR-scene-lifecycle
  skal returneres som fund, ikke løses med et lokalt reset eller en ekstra fence.
- **Leverance:** Diff, præcis reproduktion før/efter, input/buildidentitet,
  ASan/UBSan-resultat og resterende usikkerhed. Fuld racekorrekthed er særskilt.

**Accepteret 11. oktober:** Det fælles load-sted erstatter de gamle CPU-pakker
og nulstiller borrowed spans. Root genkørte actual-source ASan/UBSan både med
små fixtures og aligned car/world; fjernet rettelse fejler anden load.
Normal native check består. Hardware med separat syntetisk START gennemfører
attract→race→1800ticks→loader uden fejl. To separate 40-bils-race-startkørsler
giver 16,671/16,677 ms og hver 1813 flips/1813 vblanks efter64 uden interval
over20ms. Ingen aktiv spillerkørsel eller fuld bane/finish bevist. Evidens:
`build/dc-native-reload.FcZXUA/summary.json`. Sessionblandede overgangstider
er udeladt; indlæsningspausen består.

## D44 Audit af aktiv raceinput

**Afsluttet som læsning:** `practice.c` kalder `nm_race_scene_step`, som
forbinder spillerbevægelse, bilkollisioner og award/checkpoint-stier. Root har
eftergået workerens konkrete fund i `race_field.h:324..330`, `player.h:607..611`
og `forces.h:38..42`: `proximity_disabled=1` springer live linked-neighbor-
positioner og spillerens proximity-drag over, men ikke race-kollisionerne.
Der er ikke påvist en normal-input-dead-end for course finish. Ingen kode,
matematiktests eller hardware ændret af audit-worker.

## D45 Forbind spillerens live proximityinput

- **Formål:** Afklar og fjern den aktive caller-bypass af proximity, så den
  eksisterende shared spillerkode kan bruge racets tre linked neighbors.
- **Forudsætninger:** Accepteret D43 og læseaudit D44. Følg det faktiske
  `practice.c`→`nm_race_scene_step`→`nm_race_player_step`→`nm_player_tick_step`
  inputflow og kontroller den eksisterende neighbor-kæde; inventér callers.
- **Skriveområde:** `code/practice.c` og højst én lille kontrol af input-wiring
  eller en separat hardwareinputdiagnostic under `tests/`. Brug eksisterende
  neighbor-funktion; ingen ny proximity-matematik.
- **Uden for scope:** Ingen nye gameplay-matematiktests, ændret fysik/tick-rate,
  renderer/cache/teksturarbejde, PC-reference eller implicit course-finish.
- **Accept:** Den aktive raceinput videresender den tilsigtede proximity-state,
  uden at ændre collision phase/record flags eller kopiere rendergeometri.
  Native check og root-ejet finite hardwarekørsel med faktisk inputflow skal
  bestå. Sammenlign gameplay-hændelser og framebudget; syntetisk gas er ikke
  fysisk kalibrering eller dokumentation for et fuldt gennemført løb.
- **Stopregel:** Ukendt mening med bypass, ugyldige neighbor-links eller en
  rejected tick kræver evidens og isoleret årsagsanalyse; svæk ingen guards
  eller prøv tilfældige nye flag for at få forløbet igennem.
- **Leverance:** Mindste diff eller præcis blocker, kommando/config/input- og
  ELF-identitet, exitkode og resterende usikkerhed. Full-course og finishflow
  er den følgende milepæl, ikke automatisk dækket af 1800 ticks.

**Accepteret 11. oktober:** Den aktive inputforbindelse har ikke længere en
ubetinget proximity-bypass. Shared ring-reader og matematik er uændrede;
inhibit/penalty er nul i det nuværende normale native race. Source/oracle-
provenance og tre-neighbor-kæden er gennemgået af root. Et separat getter-
diagnostic og en lille inputkontrol er tilføjet; normal native check består.
Hardware A/B/B/A med gas255/steer0/brake0 gennemfører alle1800ticks uden fejl
eller dropped tid, holder cirka60fps og gentager terminalstates inden for hver
variant. Efter: profile145/speed-index252/pairs1235/responses158; lap/finish0.
Evidens `build/dc-native-proximity-wiring.3ZVRLb/summary.json`. Ikke et isoleret
proximity-benchmark eller full-course proof. Intet nyt gameplay-matematikcheck.

## D46 Naturligt timeout og resultforløb

Status: ikke accepteret. Hardware nåede en uventet eligible målgang ved clock1682
før timeout; D46-kontrollen afviser dette med exit1. Resultater og automatisk
navne-timeout nåede title-handoff-intercept og loader med program-exit0, men
det er ikke det efterspurgte naturlige race-timeout. Årsagskort D47 nedenfor.
Evidens `build/dc-native-natural-timeout.8AgxaL/{run-1.log,summary.json,acceptance.log}`.

- **Formål:** Gennemfør et normalt inputdrevet race indtil den virkelige timer
  udløber, følg result/name-handoff, og returnér derefter til loaderen uden at
  vise titelskærmen. Indsprøjt ikke finishbit, rang, timer eller phase.
- **Forudsætninger:** D43 asset-reload og D45 proximity er accepteret. Læs
  alle `nm_results_update`, name-stop og title-start kaldere. Genbrug det
  separate gasinput-diagnostic; almindeligt produktionsloop forbliver enDjinns.
- **Skriveområde:** `tests/race_controls_wrap.c` til test-only title-handoff-
  intercept/finite watchdog og dens eksisterende lille hostinputkontrol. Ingen
  nye product-feature flags eller gameplay-matematiktests.
- **Uden for scope:** Ingen timer-/phase-/finish-injektioner, rendererændringer,
  automatisk banestyring, permanente records/VMU eller påstand om course finish.
- **Accept:** Root ejet `AUTO_START=1`, `RACE_TICK_LIMIT=0`-hardwareforløb med
  identificeret ELF/assets går via det naturlige timerudløb, result og name-tail
  til title-handoff-intercept og loader med exit0. Bevar fysisk exit-kombination;
  en watchdog-exit er FAIL, ikke naturlig completion. Mål faktisk framebudget
  og transitions hver for sig; eksisterende sessionblandede tal må ikke kaldes
  faseopdelte performance-målinger.
- **Stopregel:** Ukendt ejerskab, assetfejl, manglende expected handoff eller
  rejected tick returneres som evidens. Ret ikke et sent skærmsymptom eller
  omgå en guard; opret et konkret årsagskort før en produktionsrettelse.
- **Leverance:** Reproducerbar kommando, flag/input-/ELF-hashes, naturlige
  phase-hændelser, terminalstate/exitkode og begrænsninger. Et time-out er et
  nødvendigt spilforløb, ikke bevis for lap/checkpoint/course-completion.

## D47 Initialisér checkpointkonfiguration ved fælles racestart

**Accepteret 11. oktober 2026.** ROM-proveniens bekræfter lap-byte og maskens
to ejere. Mindste fælles init er lagt før grid/base/rolling-reset; count32
afvises. Asset-backed ASan/UBSan wiring-check afviser den gamle nul-variant.
To ens hardware timeout-forløb når naturligt result/name/loader ved clock5210,
lap2/2 og finish0 uden injection; displayvinduet har én flip pr vblank. Dette
er ikke fuld coursecompletion, fysisk inputkalibrering eller winning/podiumtest.
Evidens `build/dc-native-race-entry.tNWeHn/{summary.json,commands.txt}`.

- **Formål:** Ret den første påviste state-afvigelse: `nm_race_scene_start`
  nulstiller scenen, men overfører ikke banens total_laps eller checkpointmasker
  til spillerens checkpoints. DNR7 course0 har total_laps8/checkpoint_count0;
  den aktuelle scene efterlader total_laps0/required0/visited0. Første lap kan
  dermed blive målgang. `nm_player_base_reset` bevarer disse eksterne felter.
- **Forudsætninger:** D46 log og kildeflow reviewed. Sammenlign originalens
  cold-entry-ejerskab med eksisterende single-car `practice.h` og alle kaldere
  af `nm_race_scene_start`; behold rolling-release/history-ejerskab adskilt.
- **Skriveområde:** `include/race_scene.h`, en lille `tests/test_race_start.c`
  kun til racestart-wiringasserts (holdes separat fra movement/math-kampagnen), og eksisterende
  diagnostic/evidens hvis en målrettet hardwarekontrol kræver det. Ingen ny
  gameplay-matematiktestkampagne eller ændring af fysiske beregninger.
- **Uden for scope:** Ingen renderer, checkpointalgoritme, faseinjektion,
  lapreduktion, kunstig steering/autopilot, performanceflags eller nye formater.
- **Accept:** Mindste fælles initialization-fix efter proveniens; startfelter
  svarer til det validerede asset. Den lille wiringkontrol afviser den gamle
  nul-init. Nativecheck passer. Root gennemfører ny naturlig hardwaretest med
  registrerede inputs/flags/hashes; første lap alene må ikke være målgang i
  8-laps-racet. Ingen ændrede forventninger for at acceptere D46's fejlede run.
- **Stopregel:** Uklart originalejerskab, ændret rolling/modstanderadfærd eller senere
  rejected tick returneres som evidens; udvid ikke til en anden algoritmerettelse.
- **Leverance:** Diff, original/caller-evidens, checkkommando/exitkode,
  asset-/ELF-identitet og hardwareforløb. Fuld fysisk coursecompletion er en
  senere accept, ikke noget initialization alene beviser.

## D48 Kontroltimer for fase 27 og 28

**Accepteret 11. oktober 2026 inden for sit scope.** `include/podium.h` ejer kun
originalens kontrolforløb: fase 27 sætter timer 1600 og falder i samme pass
igennem til fase 28; fase 28 tæller ned, rydder op ved timer 384 og vælger fase
25, når timeren er negativ. Det giver 1215 ticks med cinematiske tasks, oprydning
på tick 1216 og fase 25 efter tick 1601. `tests/test_podium.c` (ASan/UBSan,
ROM-fri, registreret i `make check`) afviser varianten med 128. Ejeren er
**ikke** koblet til `practice.c`: en vindende målgang stopper stadig ved den
eksisterende unsupported-phase-guard, indtil D49 leverer selve cinematikken.
En tom timer foran resultatskærmen ville være den genvej, D18 advarer imod.
Evidens: `build/dc-native-podium-control.pNLIOS/`.

Kildefund fra denne opgave, alle læst i de genkendte daytona93-images:

| Emne | Original | Betydning |
| --- | --- | --- |
| Oprydningstærskel | `03f54 shlo 7,3` = 384 (samme kodning giver 1600, 800/608 og 128 andre steder) | Tidligere note om 128 var forkert |
| Rækkefølge i en frame | `01758` kalder fasehandleren `01898`; derefter task-løkken `01838` | Handlerens flag gælder samme ticks tasks |
| Taskopsætning | Fase 27 aktiverer præcis samme sæt som attract-opsætningen `0197c`: b948, 9e2c, 220000, 12068, spiller, c9b0-modstandere, 6484, 5890, 4ddc, 5b68, stage-graf, 120ac | Podiet er attract-maskineriet med anden graf, ikke en ny aktørtype |
| Stage-graf | `2265a4` vælger `2865a70[bane]` og genbruger `226378`; bane 0 har 4 stages (40/40/40/10 biler, 320/320/384/384 ticks) | Samme layout som `nm_intro_stages` |
| Taskorden | Tabel `22f1f0`: biler, trafik, kamera (række 58), bil-draw, graf (række 62) | I entry-framen kører 5b68 før grafen; grafen overskriver derefter kameracursoren |
| Efter oprydning | Alle cinematiske tasks er slået fra; `5460` indlæser `2800014` | 386 ticks uden 3D-aktører; det synlige 2D-indhold er ikke verificeret |

## D49 Podiets cinematiske sekvens

Status 11. oktober aften: implementeret og kørt på hardware via røgtest
(`NATIVE_PODIUM_TEST`), ikke accepteret: en rigtig vundet målgang mangler.
Punkt 1-3 nedenfor er løst ved kilden (fasebit som scenedata og tredje
crash-katalog; nul-profil efter tabellen eksporteret; kameranøgle fjernet
offline). Punkt 4 er stadig åbent. Oprindelige forudsætninger:

1. **Fasebit 28 i stedet for 3.** `nm_intro_scene` sender literal 8
   (`1<<3`) til trafikorden, damage-prefix, drive-step og kollisionsinput, og
   crash-kataloget noterer selv, at flag 28-alternativet ligger uden for scenen.
   Hver forbruger skal læses med `0x10000000`; det er min læsning af den native
   kode, ikke en gennemført originalaudit.
2. **Spawn på profil 195.** Stage 0 placerer bil 1 og 2 på sidste profil.
   `cffc..d00c` lægger 1 til uden wrap, altså fase 196 og path-offset 196*28.
   Eksporteren afviser profil >= 195, og `nm_intro_drive_step` afviser fase >=
   196. Originalens første opdatering læser dermed efter den logiske tabel;
   find den faktiske læsning og wrap-ejer, udvid ikke en guard på gefühl.
3. **Kameranøgle med faldende tid.** Stage 0 har tiderne 0/128/320/10. I
   `5cb8..5d24` kan nøglen med tid 10 hverken blive start eller slut på et
   interval, når den ikke har callback; den kan fjernes offline med bevis.
   `nm_intro_camera_step` skal fortsat afvise uordnede tider.
4. **Bevaret tilstand fra racet.** Fase 27 nulstiller ikke bilerne, som
   attract-opsætningen gør (`39c0`/`380c` kaldes ikke); `nm_intro_scene_start`
   nulstiller alt. Afklar hvilke felter grafens spawn ikke overskriver.

- **Skriveområde når forudsætningerne er afklaret:** `tools/prepare_intro.py`
  (podiegraf i ny lokal header), `include/intro_scene.h` (stage-tabel og fasebit
  som input), `code/practice.c` og `code/render/scene/practice.c` (ejerskab og
  bilkilde), én lille lifecyclekontrol. Læs alle kaldere af `nm_intro_stages`.
- **Uden for scope:** Ingen 27->25-genvej, ingen fase-/rang-/finishinjektion i
  den endelige hardwarekørsel, ingen ny gameplay-matematikkampagne, ingen
  2D-rulletekst eller lyd.
- **Accept:** Naturlig vindende målgang når fase 27/28, viser cinematikken,
  rydder op ved 384 og fortsætter gennem resultat/navn til loaderen. Et
  diagnostic med injiceret rang må kun bruges til at finde fejl, ikke som accept.
- **Stopregel:** En forbruger, hvis flag 28-gren ikke kan læses entydigt, eller
  en læsning uden for eksporteret data, returneres som fund.

## Arbejdsinstruks til en worker

> Løs kun opgave Dxx i denne fil. Læs fælleskontrakten og det konkrete kort.
> Start med at kontrollere forudsætninger og registrere fil-/inputbaseline.
> Skeln i din levering mellem originalviden, aktuel kode og antagelser.
> Overhold skriveområdet. Ved en stopregel: lever evidens og stop.
> Ingen selvvalgte sideopgaver, hardwareadgang, nye delopgaver eller commits.
> Afslut med diff/fund, præcis kontrolkommando, input/buildidentitet, exitkode,
> evidensstier og resterende usikkerhed. Reviewer, ikke du, godkender integration.
