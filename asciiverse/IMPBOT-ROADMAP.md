# ImpBot to 1.0: the computer players

ImpBot (`Clients/ImpBot.c`) is a computer player for Imperium that logs
in over TelImp and plays by typing commands, exactly as a person does.
This is the plan for taking it to 1.0. It is written from test games on
private universes (short Imperium Time Units, four to eight computer
empires, nobody else connected) in November 2026.

Rules that hold at every stage, as they do today:

- The server is never changed for the computer players. They see only
  what their own scans and reports show, and pay the same BTUs, money,
  production and time as a person.
- ImpBot stays one self-contained C file (the C library and BSD
  sockets), GPL version 1 or later, so it can go upstream to Marisa
  Giancarla as it is.
- It plays the game the server actually runs. Where the documentation
  and the game disagree (fuel tanks, below), it follows the game.

## What "done" means for each level

**Easy** is an opponent a new player can learn against.

- It settles a handful of planets (about six), keeps money in hand and
  builds little.
- Its planets fire back when attacked, and it strikes back only at
  whoever attacked it, never first.
- It never wipes out a beginner in the first game days, and it never
  sits frozen: every session it does something a player can see (a new
  colony, a ship, a tube).
- Done when: in ten fresh universes against a passive human, an easy
  empire reaches 4 to 8 planets in two game days, never fires first, and
  fires back within a session of being attacked.

**Normal** is a fair opponent for someone who knows the game.

- It grows to a dozen planets or more, runs an officer school, spends
  its money (cash never sits above a few game days of upkeep), and keeps
  its ships fuelled.
- It attacks: once it has two crewed, armed ships and three planets it
  goes after the nearest colony of another empire, and it takes the
  planet when the defenders are gone.
- It defends: a colony that is fired on gets tubes and officers first,
  and a lost cause is given up rather than fed.
- Done when: in ten fresh universes, every normal empire that survives
  its first ship has fired on another empire by game day three, and at
  least some take a planet by force.

**Hard** is a real threat to a good player.

- Everything normal does, sooner and with more: it expands as fast as
  BTUs allow, keeps several armed ships, and attacks with the first one.
- It concentrates force (two or three ships on one target), rearms and
  refuels between strikes, and takes planets, not only damages them.
- It uses technology on purpose: research and technology shares that
  keep its weapons ahead, refurbishing old ships.
- Done when: a hard empire beats two normal empires in the same universe
  more often than not, and a human who ignores it loses colonies to it.

**1.0 for all three** also means: no computer empire is ever stuck with
money it can't spend or ships it can't move for more than a game day; a
week of play on a private universe gives no crash, no lost connection it
doesn't recover from, and no runaway BTU or log use; and Doc/impbot.doc
describes what each level does.

## Where it stands (av.6, 2026-11-15)

Trials for av.6: eight private universes at a 2 or 4-second Imperium
Time Unit (a game day in five to ten minutes), 4 to 6 computer empires
each across the three levels, against an unchanged av.5 universe run
alongside.

- **Fuel is solved.** Every colony makes fuel tanks (the game does not
  enforce the air and water rule in doc items). In the av.5 universe,
  three of four empires were still at one or two planets after 45
  sessions, two of them with a ship stranded for want of fuel. With
  av.6, normal and hard empires that keep their first ship reach 5 to
  11 planets and 5 to 12 ships by session 21, and 13 to 16 planets and
  18 to 23 ships by session 45; easy stops at about six planets and
  three ships, as it should.
- **Money is spent, but not all of it.** Planets drop from a third cash
  to a few percent as money builds up, and the money goes into tubes and
  ships (a planet can build only three things a session; ships are now
  assembled from parts built ahead), and gold mining stops. Normal and
  hard still hold $400,000 to $1,100,000 by session 45 (av.5's two
  growing empires had $3,900,000 and $6,000,000 by session 70, the two
  stuck ones $640,000 to $820,000 with one or two planets). Most of the
  income is interest on gold bars and the stockpiles of production are
  large, so it is outlets, not production, that run out: ship and tube
  numbers are capped, and nothing else is bought. Easy keeps its cash on
  purpose and ends up with over a million.
- **Officers: the game makes them hard.** Officer school runs only on a
  planet whose soldiers are more than 3% of the most people it can hold
  and whose officers are at least 5% of its soldiers and officers;
  soldiers are trained only where officers are at least 1% of the
  civilians. With five officers that means a small planet (size 0 or 1)
  with fewer than about 600 people, ore on hand, and soldiers trained in
  its first updates; a new colony goes from 60 people to thousands in
  three sessions. ImpBot tries it when its first colony is small (it
  settles with 60 people, keeps the planet out of its other updates and
  keeps the soldiers in the band where officers are trained), but in
  every trial the people outgrew the window before any soldiers were
  trained. What does work: the five officers stay on the first colony
  (kept aboard the first ship, they were lost with it to stars and empty
  tanks), and ships carry them to the warships, then to planets with
  tubes and to planets under attack.
- **Attacking works for normal now.** In the last two trial universes
  (2-second ITU, twelve empires), four normal empires fired on other
  empires' colonies between sessions 25 and 30, one of them seven times;
  two of the targets learnt who had attacked them from their telegrams.
  Most torpedoes miss (one hit in eighteen shots) and no planet was taken
  by force. Contact is the slow part: an empire knows another's colony
  only after one of its ships has flown over it, and in a 100 by 100
  subsector galaxy that takes most of a game day.
- **Still open:** most first ships are lost flying into stars (a single
  sensor array can't tell a star from a planet; four in ten unknown
  planets are stars), so about half the empires restart once or twice;
  defence is passive (tubes and officers to a planet that was hit, no
  ships sent); a colony given up is only no longer fed; a ship that
  keeps stopping short of where it is sent tries again every session.

## What is left, in the order to do it

1. **First ship survival** (all levels). The single biggest loss. Ideas
   that stay within what a person can do: settle the first planet seen
   closely rather than flying onto unknown ones; scan from the home
   planet's sector before leaving; accept a slower start for a safer one
   on easy and normal.
2. **Officer school that starts reliably** (normal, hard). The av.6
   attempt shows the window is real but short. Next: find out exactly
   which commands bring a planet up to date (each one lets its people
   double), choose the school planet before settling it (size 0 or 1,
   high water), carry ore and soldiers from an existing colony, settle
   with as few people as the game allows, divvy in the same visit, and
   use a ship to carry people away whenever the school outgrows its
   window. Measure: a school with 15 officers by game day two in most
   universes. Without it, normal and hard stay limited to five crewed
   ships or planets.
3. **War that finishes the job** (normal, hard). Find neighbours sooner
   (explore toward other races' home planets, read `power` and the
   telegrams); send two or three armed ships together; fire until the
   defenders are gone; land settlers and take the planet; go back to
   rearm and refuel. Raise technology before building tubes, since most
   torpedoes miss.
4. **Active defence** (all levels). Answer an attack with the nearest
   armed ships; fire on enemy ships in our sectors; move soldiers to a
   threatened colony; evacuate people from one that can't be held.
5. **New ways to spend** (normal, hard). Refurbish old ships to the
   planet's technology, shields and blasers, robot miners (`miner`),
   loans to allies, and larger ship classes; lift the caps on ships and
   tubes for an empire that can crew them. Measure: cash stays under a few game days of
   upkeep.
6. **Stranded ships** (all levels). Rescue a ship out of fuel by
   bringing it fuel tanks (`tend` moves small items between ships in
   space) instead of writing it off, and stop sending a ship along a
   path it keeps stopping short on.
7. **Talking** (later, optional). Answer telegrams in a few set ways,
   declare alliances and war (`declare`), trade.
8. **Hardening for 1.0.** A week-long run on a private universe with
   seven computer empires and a person; log size and BTU use checked;
   reconnects after a server restart; Doc/impbot.doc brought up to date;
   then offer ImpBot upstream to Marisa.

Easy is nearly done after items 1 and 4; normal after 1 to 4; hard after
all of 1 to 6.
