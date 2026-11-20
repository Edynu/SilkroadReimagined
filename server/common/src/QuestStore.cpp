#include <algorithm>
#include "sr/server/QuestStore.h"

#include <map>

#include "sr/server/QuestScript.h"

namespace sr::server {

namespace {

bool Fail(const QueryResult& r, std::string& error) {
    if (r.Ok()) return false;
    error = r.Error();
    return true;
}

bool TableExists(Database& db, const char* name) {
    const QueryResult r = db.Query("SELECT to_regclass($1) IS NOT NULL", {name});
    return r.Ok() && r.Rows() == 1 && r.Bool(0, 0);
}

// A text key of the original's tables ("xxx" and empty: none).
game::QuestText Key(const std::string& s) { return s.empty() || s == "xxx" ? game::QuestText{} : game::TextKey(s); }

} // namespace

bool ImportQuestScript(Database& db, std::span<const std::uint8_t> script, QuestImportReport& report, std::string& error) {
    report = {};
    std::vector<game::QuestDef> quests;
    if (!DecodeQuestScript(script, quests, report.notes, error)) return false;
    report.decoded = int(quests.size());
    // The quests the game server codes itself, beside the script's (OriginalQuests.cpp).
    std::vector<game::QuestDef> coded;
    if (!DecodeCodedQuests(coded, error)) return false;
    report.coded = int(coded.size());
    for (game::QuestDef& q : coded) {
        // One the script has too (its talk or handlers coded in the script itself: written in our format instead)
        // takes the script's place.
        const auto same = std::find_if(quests.begin(), quests.end(), [&](const game::QuestDef& s) { return s.codename == q.codename; });
        if (same != quests.end()) {
            *same = std::move(q);
            ++report.replaced;
        } else {
            quests.push_back(std::move(q));
        }
    }
    if (!TableExists(db, "_refquest")) {
        error = "the original's _RefQuest is not in the database (import its tables first)";
        return false;
    }
    // _RefQuest: the quests in service, their numbers, levels and texts.
    struct Ref {
        std::uint32_t id = 0;
        int level = 0;
        std::string name, pay, payText, guideNpc, guideCondition;
    };
    std::map<std::string, Ref> refs;
    {
        const QueryResult q = db.Query(
            "SELECT id, codename, level, namestring, paystring, paycontents, noticenpc, noticecondition FROM _refquest WHERE service = 1");
        if (Fail(q, error)) return false;
        for (int i = 0; i < q.Rows(); ++i)
            refs[q.Text(i, 1)] = {std::uint32_t(q.Int(i, 0)), int(q.Int(i, 2)), q.Text(i, 3), q.Text(i, 4), q.Text(i, 5), q.Text(i, 6), q.Text(i, 7)};
    }
    // _RefQuestReward and its items (kind 0: the items given; the other kinds belong to quests the server codes itself).
    std::map<std::string, game::QuestReward> rewards;
    if (TableExists(db, "_refquestreward")) {
        const QueryResult q = db.Query(
            "SELECT questcodename, gold, exp, spexp, sp, ap, aptype, hwan, inventory, isview, ischeckcountry, selectioncnt "
            "FROM _refquestreward WHERE service = 1");
        if (Fail(q, error)) return false;
        for (int i = 0; i < q.Rows(); ++i) {
            game::QuestReward& r = rewards[q.Text(i, 0)];
            r.gold = std::uint64_t(q.Int(i, 1));
            r.exp = std::uint64_t(q.Int(i, 2));
            r.skillExp = std::uint64_t(q.Int(i, 3));
            r.sp = std::uint32_t(q.Int(i, 4));
            if (q.Int(i, 5) != 0) r.points.push_back({std::uint32_t(q.Int(i, 5)), q.Text(i, 6)});
            r.hwan = std::uint32_t(q.Int(i, 7));
            r.inventory = std::uint32_t(q.Int(i, 8));
            r.shown = q.Int(i, 9) != 0;
            r.byCountry = q.Int(i, 10) != 0;
            r.choose = std::uint32_t(q.Int(i, 11));
        }
        if (TableExists(db, "_refquestrewarditems")) {
            const QueryResult items = db.Query(
                "SELECT questcodename, itemcodename, achievequantity FROM _refquestrewarditems WHERE service = 1 AND rewardtype = 0 "
                "ORDER BY questid, itemcodename");
            if (Fail(items, error)) return false;
            for (int i = 0; i < items.Rows(); ++i)
                if (auto it = rewards.find(items.Text(i, 0)); it != rewards.end())
                    it->second.items.push_back({items.Text(i, 1), std::uint32_t(items.Int(i, 2))});
        }
    }
    // Kind 4: an exchange's rows (QSP_ALL_POTION_3: OptionalItemCode, OptionalItemCnt of it, AchieveQuantity skill
    // points).
    std::map<std::string, std::vector<game::QuestDef::Tier>> tiers;
    if (TableExists(db, "_refquestrewarditems")) {
        const QueryResult t = db.Query(
            "SELECT questcodename, optionalitemcode, optionalitemcnt, achievequantity FROM _refquestrewarditems WHERE service = 1 AND rewardtype = 4 "
            "ORDER BY questid, optionalitemcnt");
        if (Fail(t, error)) return false;
        for (int i = 0; i < t.Rows(); ++i) {
            game::QuestDef::Tier tier;
            tier.item = t.Text(i, 1);
            tier.count = std::uint32_t(std::max<std::int64_t>(0, t.Int(i, 2)));
            tier.sp = std::uint32_t(std::max<std::int64_t>(0, t.Int(i, 3)));
            tiers[t.Text(i, 0)].push_back(std::move(tier));
        }
    }
    return db.Transaction(
        [&](Database& tx) {
            if (Fail(tx.Query("DELETE FROM sr_quests WHERE origin = $1", {int(QuestOrigin::Script)}), error)) return false;
            for (game::QuestDef& q : quests) {
                const auto ref = refs.find(q.codename);
                if (ref == refs.end()) {
                    ++report.unknown;
                    continue;
                }
                q.id = ref->second.id;
                q.level = ref->second.level;
                // A coded quest may name itself (QSP_ALL_POTION_3: its NameString has no words in 1.188's tables).
                if (q.name.Empty()) q.name = Key(ref->second.name);
                q.rewardTitle = Key(ref->second.pay);
                q.rewardText = Key(ref->second.payText);
                q.guideNpc = Key(ref->second.guideNpc);
                q.guideCondition = Key(ref->second.guideCondition);
                if (const auto r = rewards.find(q.codename); r != rewards.end()) {
                    // A coded quest's own choice kept where the table has none (its class picks by the talk's row:
                    // QNO_EU_FERRY2_1's payment, one of its two items).
                    const std::uint32_t choose = q.reward.choose;
                    q.reward = r->second;
                    if (q.reward.choose == 0) q.reward.choose = choose;
                    ++report.rewardsFromDatabase;
                }
                // The exchange's numbers the database's (a coded row keeps its words at the NPC).
                if (const auto t = tiers.find(q.codename); t != tiers.end()) {
                    for (std::size_t k = 0; k < t->second.size(); ++k) {
                        if (k < q.tiers.size()) {
                            q.tiers[k].item = t->second[k].item;
                            q.tiers[k].count = t->second[k].count;
                            q.tiers[k].sp = t->second[k].sp;
                        } else {
                            q.tiers.push_back(t->second[k]);
                        }
                    }
                }
                if (!SaveQuest(tx, q, QuestOrigin::Script, error)) return false;
                ++report.imported;
            }
            return true;
        },
        error);
}

bool HasScriptQuests(Database& db) {
    const QueryResult q = db.Query("SELECT count(*) FROM sr_quests WHERE origin = $1", {int(QuestOrigin::Script)});
    return q.Ok() && q.Rows() == 1 && q.Int(0, 0) > 0;
}

bool LoadQuests(Database& db, std::vector<StoredQuest>& out, std::vector<std::string>& problems, std::string& error) {
    out.clear();
    const QueryResult q = db.Query("SELECT codename, origin, definition FROM sr_quests ORDER BY id");
    if (Fail(q, error)) return false;
    for (int i = 0; i < q.Rows(); ++i) {
        StoredQuest s;
        s.origin = QuestOrigin(q.Int(i, 1));
        std::string why;
        if (!game::ParseQuest(q.View(i, 2), s.def, why)) {
            problems.push_back(q.Text(i, 0) + ": " + why);
            continue;
        }
        out.push_back(std::move(s));
    }
    return true;
}

bool SaveQuest(Database& db, const game::QuestDef& q, QuestOrigin origin, std::string& error) {
    const QueryResult r = db.Query(
        "INSERT INTO sr_quests (codename, id, origin, definition) VALUES ($1, $2, $3, $4) "
        "ON CONFLICT (codename) DO UPDATE SET id = excluded.id, origin = excluded.origin, definition = excluded.definition, updated = localtimestamp",
        {q.codename, std::int64_t(q.id), int(origin), game::WriteQuest(q)});
    return !Fail(r, error);
}

bool AddCharRecord(Database& db, std::int64_t character, std::string_view name, int n, std::string& error) {
    QueryResult r = db.Query("INSERT INTO sr_char_records (charid, name, count) VALUES ($1, $2, $3) "
                             "ON CONFLICT (charid, name) DO UPDATE SET count = sr_char_records.count + excluded.count",
                             {character, std::string(name), n});
    if (!r.Ok()) error = r.Error();
    return r.Ok();
}

bool LoadCharRecords(Database& db, std::int64_t character, std::map<std::string, int>& out, std::string& error) {
    out.clear();
    QueryResult r = db.Query("SELECT name, count FROM sr_char_records WHERE charid = $1", {character});
    if (!r.Ok()) {
        error = r.Error();
        return false;
    }
    for (int k = 0; k < r.Rows(); ++k) out[std::string(r.View(k, 0))] = int(r.Int(k, 1));
    return true;
}

} // namespace sr::server
