#ifndef FUZZY_SW_HPP_
#define FUZZY_SW_HPP_
#include <string_view>
#include <memory>
#include <vector>

namespace fuzzy_sw
{
    struct Config
    {
        int extend_gap_penalty = -1;
        int open_gap_penalty = -2;
        int mismatch_penalty = -1;

        int delimiter_boundary_bonus = 1;
        int full_match_bonus = 3;
        int icase_match_bonus = 2;
    };

    struct CharSource
    {
        virtual ~CharSource() {}
        virtual size_t length() const = 0;
        virtual size_t read(char *pDest, size_t off, size_t n) const = 0;
    };

    //non-parallel, simple version, no backtracking
    int match(std::string_view const& query, std::string_view const& target, Config const& cfg = {});
    int match(std::string_view const& query, CharSource *target, Config const& cfg = {});

    class SIMDParMatcher
    {
        struct Impl;
    public:
        SIMDParMatcher(Config const& cfg = {});
        ~SIMDParMatcher();

        using Input = std::vector<std::string_view>;
        struct ResultItem
        {
            std::string_view target;
            int score = 0;

            ResultItem() = default;
            ResultItem(std::string_view sv, int sc): target(sv), score(sc){}

            bool operator==(ResultItem const& rhs) const 
            {
                return score == rhs.score && target == rhs.target;
            }
        };
        using Result = std::vector<ResultItem>;

        using CharSourceInput = std::vector<CharSource*>;
        struct CharSourceResultItem
        {
            CharSource* target;
            int score;

            CharSourceResultItem() = default;
            CharSourceResultItem(CharSource*sv, int sc): target(sv), score(sc){}

            bool operator==(CharSourceResultItem const&rhs) const
            {
                return score == rhs.score && target == rhs.target;
            }
        };
        using CharSourceResult = std::vector<CharSourceResultItem>;

        struct Param
        {
            int scoreThreshold = 0;
            bool sortResults = false;
        };

        void SetupThreads(int threadCount);
        void StopThreads();

        Result match(std::string_view const&query, Input &&targets, Param const& params);
        Result match_par(std::string_view const&query, Input &&targets, Param const& params);

        CharSourceResult match(std::string_view const&query, CharSourceInput &&targets, Param const& params);
        CharSourceResult match_par(std::string_view const&query, CharSourceInput &&targets, Param const& params);
    private:
        Config m_Config;
        std::unique_ptr<Impl> m_Impl;
    };
}
#endif
