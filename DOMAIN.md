# Savas domenas už 0 €

Tikslas – parduotuvė veikia adresu **https://unemployed.lt.eu.org**, o senas
`unemployed-6c7e.onrender.com` automatiškai peradresuoja į jį.

| Kas | Kur | Kaina |
|---|---|---|
| Domenas | nic.eu.org (`*.lt.eu.org`) | 0 € |
| DNS | dns.he.net arba Cloudflare Free | 0 € |
| Svetainė ir HTTPS sertifikatas | Render (plan: free) | 0 € |

Caddy ar kito proxy nereikia – HTTPS sertifikatą (Let's Encrypt) Render išduoda
ir atnaujina pats.

> **Svarbu dėl eu.org.** Ši paslauga skirta **nekomerciniams projektams**.
> Parduotuvė yra komercinė veikla, todėl eu.org prašymą gali atmesti arba
> domeną vėliau išjungti. Jei projektas rimtas, ilgainiui patikimiau bus
> nusipirkti `.lt` domeną – tada žingsniai nuo 3 iki 6 lieka tie patys.

Žingsnių **eilė svarbi**: eu.org prašymo metu tikrina, ar vardų serveriai jau
atsako, todėl DNS zona sukuriama pirmiau, o `CANONICAL_HOST` įrašomas paskutinis.

---

## 1. Sukurk DNS zoną (pirmiau už domeno prašymą)

Pasirink vieną.

### A variantas: Hurricane Electric – dns.he.net (rekomenduoju)

1. Užsiregistruok https://dns.he.net (nemokama).
2. **Add a new domain** → `unemployed.lt.eu.org`. Įspėjimą, kad domenas dar
   nedeleguotas, ignoruok – deleguosi 2 žingsnyje.
3. Vardų serveriai, kuriuos įrašysi eu.org formoje:
   `ns1.he.net`, `ns2.he.net`, `ns3.he.net`, `ns4.he.net`, `ns5.he.net`.

### B variantas: Cloudflare Free

1. https://dash.cloudflare.com → **Add a site** → `unemployed.lt.eu.org` → Free planas.
   Dažniausiai tai veikia, nes `lt.eu.org` yra Public Suffix sąraše. Jei
   Cloudflare subdomeno nepriimtų, naudok A variantą.
2. Cloudflare parodys du vardų serverius (pvz. `xxx.ns.cloudflare.com`) –
   juos įrašysi eu.org formoje.

## 2. Gauk domeną nic.eu.org

1. https://nic.eu.org → **Sign up**: susikurk kontaktą (gausi „handle“, pvz. `AB123-FREE`)
   ir patvirtink el. paštą.
2. Prisijungęs → **New domain** → `unemployed.lt.eu.org`.
3. Įrašyk 1 žingsnio vardų serverius. Pažymėk **„On error: do not submit“**:
   forma iškart patikrins, ar serveriai atsako, ir neišsiųs klaidingo prašymo.
4. Prašymas patenka į eilę. **Patvirtinimas gali užtrukti iki mėnesio.** Kai
   domenas patvirtinamas, ateina laiškas.

Patikrinti galima ir pačiam:

```bash
dig NS unemployed.lt.eu.org +short
```

## 3. Sukurk DNS įrašus, rodančius į Render

Render paslaugos adresas: **`unemployed-6c7e.onrender.com`**.

DNS standartas neleidžia CNAME įrašo pačioje zonos šaknyje
(`unemployed.lt.eu.org`). Abu tiekėjai tai išsprendžia:

| Vardas | dns.he.net | Cloudflare | Reikšmė |
|---|---|---|---|
| `unemployed.lt.eu.org` (šaknis) | **ALIAS** | **CNAME** (Cloudflare jį „suplokština“) | `unemployed-6c7e.onrender.com` |
| `www.unemployed.lt.eu.org` | CNAME | CNAME | `unemployed-6c7e.onrender.com` |

- Cloudflare abiem įrašams nustatyk **Proxy status: DNS only** (pilkas
  debesėlis). Kitaip Render negalės patvirtinti domeno ir išduoti sertifikato.
- Jei tavo DNS neturi nei ALIAS, nei CNAME šaknyje, naudok **A** įrašą su
  IP adresu, kurį rodo Render (3 žingsnio lange). Paskutinį kartą jis buvo
  `216.24.57.1`, bet visada įrašyk tai, ką rodo Render.

## 4. Pridėk domeną Render'yje

1. https://dashboard.render.com → paslauga **unemployed** → **Settings** →
   **Custom Domains** → **Add Custom Domain**.
2. Įrašyk `unemployed.lt.eu.org`. Render paprastai pasiūlo pridėti ir `www`.
3. Spausk **Verify**. Kai būsena tampa **Verified** ir sertifikatas
   **Issued**, adresas https://unemployed.lt.eu.org jau veikia.

Vietoj 1–3 punktų galima atkomentuoti `domains:` bloką `render.yaml` faile –
Blueprint pridės domeną pats.

## 5. Tik tada įjunk `CANONICAL_HOST`

Kai domenas Render'yje yra **Verified** ir atsidaro per HTTPS:

1. `render.yaml` faile pakeisk:
   ```yaml
   - key: CANONICAL_HOST
     value: unemployed.lt.eu.org
   ```
2. Padaryk commit ir push į `main` – Render perdiegs pats.

Keisk `render.yaml`, o ne Render nustatymų skiltį „Environment“: kitaip kitas
Blueprint sinchronizavimas reikšmę gali vėl perrašyti tuščia.

Ką daro serveris (`src/server.c`):
- užklausa su kitu `Host` (pvz. `unemployed-6c7e.onrender.com` arba `www.…`)
  gauna **301** į `https://unemployed.lt.eu.org` su tuo pačiu keliu ir query;
- `/healthz` visada atsako `200 ok`, todėl Render health check'as veikia per bet kurį adresą;
- kol `CANONICAL_HOST` tuščias, `Host` antraštė ignoruojama ir viskas veikia kaip anksčiau.

**Kodėl ne anksčiau?** Jei `CANONICAL_HOST` įrašysi, kol domenas dar
neveikia, visi lankytojai iš `onrender.com` bus nukreipti į neveikiantį adresą.

## 6. Patikra

```bash
curl -sI https://unemployed-6c7e.onrender.com/ | grep -iE '^(HTTP|location)'
```

```bash
curl -s https://unemployed.lt.eu.org/healthz
```

Pirmoji komanda turi grąžinti `301` ir `location: https://unemployed.lt.eu.org/`,
antroji – `ok`.

**Atšaukimas:** `CANONICAL_HOST` vėl padaryk `""`, padaryk commit ir push.
