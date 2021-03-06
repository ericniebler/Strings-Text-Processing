// Regular expression implementation.
// Supports traditional egrep syntax, plus non-greedy operators.
// Tracks submatches a la traditional backtracking.
// 
// Finds leftmost-biased (traditional backtracking) match;
//
// Executes repetitions likt Perl.
//
// Requires Boost C++ Libraries, see http://boost.org
//
// g++ -I $BOOST_ROOT nfa-perl.cpp
//	a.out '(a*)+' aaa           # (0,3)(3,3)
//	a.out '(a|aa)(a|aa)' aaa    # (0,2)(0,1)(1,2)
// 
// Copyright (c) 2007 Russ Cox.
// Copyright (c) 2011 Eric Niebler.
// Can be distributed under the Boost Softwate License 1.0, see bottom of file.

#define BOOST_SPIRIT_DEBUG
#include <cstring>
#include <deque>
#include <vector>
#include <utility>
#include <iostream>
#include <iomanip>
#include <boost/config/warning_disable.hpp>
#include <boost/array.hpp>
#include <boost/next_prior.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/spirit/include/qi.hpp>
#include <boost/spirit/include/phoenix_core.hpp>
#include <boost/spirit/include/phoenix_operator.hpp>
#include <boost/spirit/include/phoenix_object.hpp>

namespace qi = boost::spirit::qi;
namespace ascii = boost::spirit::ascii;
namespace phoenix = boost::phoenix;

enum
{
    LeftmostBiased = 0,
    LeftmostLongest = 1,
};

enum
{
    RepeatMinimal = 0,
    RepeatLikePerl = 1,
};

int debug;
int matchtype = LeftmostBiased;
int reptype = RepeatMinimal;

enum SubState
{
    Unmatched = 0,
    Incomplete = 1,
    Matched = 2
};

enum
{
    NSUB = 10
};

template<typename Iter>
struct Sub
  : std::pair<Iter, Iter>
{
    Sub(Iter first_=Iter(), Iter second_=Iter())
      : std::pair<Iter, Iter>(first_, second_)
      , matched(Unmatched)
    {}

    Sub &operator=(Sub const &sub)
    {
        // don't copy singular iterators
        switch(matched = sub.matched)
        {
        case Matched:    second = sub.second;
        case Incomplete: first  = sub.first;
        default:;
        }
        return *this;
    }

    SubState matched;
};

enum
{
    Char = 1,
    Any = 2,
    Split = 3,
    LParen = 4,
    RParen = 5,
    Match = 6,
};

struct State
{
    int op;
    int data;
    State const *out;
    State const *out1;
    std::size_t id;

private:
    friend struct REImpl;
    explicit State(int op_, int data_, std::size_t id_, State const *out_, State const *out1_)
      : op(op_), data(data_), out(out_), out1(out1_), id(id_)
    {}
};

template<typename Iter>
struct Thread;

template<typename Iter>
struct StateEx
{
    StateEx()
      : lastlist(0), visits(0), lastthread(0)
    {}

    int lastlist;
    int visits;
    Thread<Iter> *lastthread;
};

template<typename Iter>
struct Extras
{
    Extras(std::size_t nstates)
      : listid(0), stateex(nstates + 1, StateEx<Iter>())
    {}
    
    int listid;
    std::vector<StateEx<Iter> > stateex;
};

template<typename Iter>
struct Thread
{
    State const *state;
    boost::array<Sub<Iter>, NSUB> match;
};

template<typename Iter>
struct List
{
    explicit List(std::size_t nstates)
      : t(nstates, Thread<Iter>()), n(0)
    {}

    std::vector<Thread<Iter> > t;
    int n;
};

struct REImpl
{
    REImpl()
      : start(0), nparen(0), states(new std::deque<State>)
    {}

    State *state(int op, int data, State const *out=0, State const *out1=0)
    {
        states->push_back(State(op, data, states->size()+1, out, out1));
        return &states->back();
    }

    void dump()
    {
        std::vector<bool> seen(states->size() + 1);
        dump(start, seen);
    }

    void dump(State const *s, std::vector<bool> &seen)
    {
        if(s == 0 || seen[s->id])
            return;
        seen[s->id] = true;
        std::cout << s->id << "| ";

        switch(s->op)
        {
        case Char:
            std::cout << '\'' << (char)s->data << "' -> " << s->out->id << '\n';
            break;

        case Any:
            std::cout << ". -> " << s->out->id << '\n';
            break;

        case Split:
            std::cout << "| -> " << s->out->id << ", " << s->out1->id << '\n';
            break;

        case LParen:
            std::cout << "( " << s->data << " -> " << s->out->id << '\n';
            break;

        case RParen:
            std::cout << ") " << s->data << " -> " << s->out->id << '\n';
            break;

        case Match:
            std::cout << "match\n";
            break;

        default:
            std::cout << "??? " << s->op << '\n';
            break;
        }

        dump(s->out, seen);
        dump(s->out1, seen);
    }

    friend std::ostream &operator <<(std::ostream &sout, REImpl const &reimpl)
    {
        return sout << "REImpl";
    }

    State const *start;
    int nparen;
    boost::shared_ptr<std::deque<State> > states;
};

// Since the out pointers in the list are always
// uninitialized, we use the pointers themselves
// as storage for the Ptrlists.
union Ptrlist
{
    Ptrlist *next;
    State const *s;
};

struct Frag
{
    explicit Frag(State const *start_=0, Ptrlist *out_=0)
      : start(start_), out(out_)
    {}

    State const *start;
    Ptrlist *out;

    friend std::ostream &operator <<(std::ostream &sout, Frag const &frag)
    {
        return sout << "Frag";
    }
};

// Create singleton list containing just outp.
Ptrlist *list1(State const **outp)
{
    Ptrlist *l = (Ptrlist*)outp;
    l->next = 0;
    return l;
}

// Patch the list of states at out to point to start.
void patch(Ptrlist *l, State const *s)
{
    for(Ptrlist *next; l; l=next)
    {
        next = l->next;
        l->s = s;
    }
}

// Join the two lists l1 and l2, returning the combination.
Ptrlist *append(Ptrlist *l1, Ptrlist *l2)
{
    Ptrlist *oldl1 = l1;
    while(l1->next)
    {
        l1 = l1->next;
    }
    l1->next = l2;
    return oldl1;
}

struct frag1_result
{
    template<typename>
    struct result
    {
        typedef Frag type;
    };
};

struct frag2_result
{
    template<typename, typename>
    struct result
    {
        typedef Frag type;
    };
};

struct frag3_result
{
    template<typename, typename, typename>
    struct result
    {
        typedef Frag type;
    };
};

struct any_char_impl : frag1_result
{
    Frag operator()(REImpl &impl) const
    {
        State *s = impl.state(Any, 0);
        return Frag(s, list1(&s->out));
    }
};

struct single_char_impl : frag2_result
{
    Frag operator()(REImpl &impl, char ch) const
    {
        State *s = impl.state(Char, ch);
        return Frag(s, list1(&s->out));
    }
};

struct paren_impl : frag3_result
{
    Frag operator()(REImpl &impl, Frag f, int n) const
    {
        if(n >= NSUB)
            return f;
        State *s1 = impl.state(LParen, n, f.start, 0);
        State *s2 = impl.state(RParen, n, 0, 0);
        patch(f.out, s2);
        return Frag(s1, list1(&s2->out));
    }
};

struct greedy_star_impl : frag2_result
{
    Frag operator()(REImpl &impl, Frag f) const
    {
        State *s = impl.state(Split, 0, f.start, 0);
        patch(f.out, s);
        return Frag(s, list1(&s->out1));
    }
};

struct non_greedy_star_impl : frag2_result
{
    Frag operator()(REImpl &impl, Frag f) const
    {
        State *s = impl.state(Split, 0, 0, f.start);
        patch(f.out, s);
        return Frag(s, list1(&s->out));
    }
};

struct greedy_plus_impl : frag2_result
{
    Frag operator()(REImpl &impl, Frag f) const
    {
        State *s = impl.state(Split, 0, f.start, 0);
        patch(f.out, s);
        return Frag(f.start, list1(&s->out1));
    }
};

struct non_greedy_plus_impl : frag2_result
{
    Frag operator()(REImpl &impl, Frag f) const
    {
        State *s = impl.state(Split, 0, 0, f.start);
        patch(f.out, s);
        return Frag(f.start, list1(&s->out));
    }
};

struct greedy_opt_impl : frag2_result
{
    Frag operator()(REImpl &impl, Frag f) const
    {
        State *s = impl.state(Split, 0, f.start, 0);
        return Frag(s, append(f.out, list1(&s->out1)));
    }
};

struct non_greedy_opt_impl : frag2_result
{
    Frag operator()(REImpl &impl, Frag f) const
    {
        State *s = impl.state(Split, 0, 0, f.start);
        return Frag(s, append(f.out, list1(&s->out)));
    }
};

struct do_concat_impl : frag2_result
{
    Frag operator()(Frag f1, Frag f2) const
    {
        patch(f1.out, f2.start);
        return Frag(f1.start, f2.out);
    }
};

struct do_alt_impl : frag3_result
{
    Frag operator()(REImpl &impl, Frag f1, Frag f2) const
    {
        State *s = impl.state(Split, 0, f1.start, f2.start);
        return Frag(s, append(f1.out, f2.out));
    }
};

struct next_paren_impl
{
    template<typename>
    struct result
    {
        typedef int type;
    };

    int operator()(REImpl &impl) const
    {
        return ++impl.nparen;
    }
};

struct do_regex_impl
{
    template<typename, typename>
    struct result
    {
        typedef void type;
    };

    void operator()(REImpl &impl, Frag f) const
    {
        f = paren_impl()(impl, f, 0);
        State *s = impl.state(Match, 0, 0, 0);
        patch(f.out, s);
        impl.start = f.start;
    }
};

phoenix::function<any_char_impl> const any_char = any_char_impl();
phoenix::function<single_char_impl> const single_char = single_char_impl();
phoenix::function<paren_impl> const paren = paren_impl();
phoenix::function<greedy_star_impl> const greedy_star = greedy_star_impl();
phoenix::function<non_greedy_star_impl> const non_greedy_star = non_greedy_star_impl();
phoenix::function<greedy_plus_impl> const greedy_plus = greedy_plus_impl();
phoenix::function<non_greedy_plus_impl> const non_greedy_plus = non_greedy_plus_impl();
phoenix::function<greedy_opt_impl> const greedy_opt = greedy_opt_impl();
phoenix::function<non_greedy_opt_impl> const non_greedy_opt = non_greedy_opt_impl();
phoenix::function<do_concat_impl> const do_concat = do_concat_impl();
phoenix::function<do_alt_impl> const do_alt = do_alt_impl();
phoenix::function<do_regex_impl> const do_regex = do_regex_impl();
phoenix::function<next_paren_impl> const next_paren = next_paren_impl();

template<typename Iter>
struct regex_grammar
  : qi::grammar<Iter, void(REImpl&)>
{
    regex_grammar()
      : regex_grammar::base_type(regex, "regex grammar")
    {
        using qi::eps;
        using ascii::char_;
        using qi::on_error;
        using qi::fail;
        using qi::debug;
        using namespace qi::labels;

        regex  = alt(_r1)[ do_regex(_r1, _1) ];

        alt    = concat(_r1)[ _val = _1 ]
                        >> *('|' >> concat(_r1)[ _val = do_alt(_r1, _val, _1) ]);

        concat = repeat(_r1)[ _val = _1 ]
                        >> *(repeat(_r1)[ _val = do_concat(_val, _1) ]);

        repeat = single(_r1)[ _val = _1 ]
                        >> -( (char_('*') >> '?')[ _val = non_greedy_star(_r1, _val) ]
                            | (char_('+') >> '?')[ _val = non_greedy_plus(_r1, _val) ]
                            | (char_('?') >> '?')[ _val = non_greedy_opt(_r1, _val) ]
                            | (char_('*'))[ _val = greedy_star(_r1, _val) ]
                            | (char_('+'))[ _val = greedy_plus(_r1, _val) ]
                            | (char_('?'))[ _val = greedy_opt(_r1, _val) ])
                        ;

        count  = eps[ _val = next_paren(_r1) ];

        single = char_('(') >> '?' >> ':' >> alt(_r1)[ _val = _1 ] >> ')'
                | (char_('(') >> count(_r1)[ _a = _1 ] >> alt(_r1)[ _val = _1 ] >> ')')
                    [
                        _val = paren(_r1, _val, _a)
                    ]
                | char_('.') [ _val = any_char(_r1) ]
                | (~char_("|*+?():."))[ _val = single_char(_r1, _1)]
                ;

        using phoenix::val;
        using phoenix::construct;

        on_error<fail>
        (
            regex
          , std::cout
                << val("ERROR: Expecting ")
                << _4                               // what failed?
                << val(" here: \"")
                << construct<std::string>(_3, _2)   // iterators to error-pos, end
                << val("\"")
                << std::endl
        );

        BOOST_SPIRIT_DEBUG_NODE(regex);
        BOOST_SPIRIT_DEBUG_NODE(alt);
        BOOST_SPIRIT_DEBUG_NODE(concat);
        BOOST_SPIRIT_DEBUG_NODE(repeat);
        BOOST_SPIRIT_DEBUG_NODE(single);
    }

    qi::rule<Iter, void(REImpl&)> regex;
    qi::rule<Iter, Frag(REImpl&)> alt, concat, repeat;
    qi::rule<Iter, int(REImpl&)> count;
    qi::rule<Iter, Frag(REImpl&), qi::locals<int> > single;
};

// Is match a longer than match b?
// If so, return 1; if not, 0.
template<typename Iter>
int longer(boost::array<Sub<Iter>, NSUB> const &a, boost::array<Sub<Iter>, NSUB> const &b)
{
    if(a[0].matched == Unmatched)
        return 0;
    if(b[0].matched == Unmatched || a[0].first < b[0].first)
        return 1;
    if(a[0].first == b[0].first && a[0].second > b[0].second)
        return 1;
    return 0;
}

template<typename Iter>
struct Matcher
{
    Matcher(REImpl const &impl_)
      : impl(impl_), begin(), end(), subs(), dummy()
      , l1(impl.states->size()), l2(impl.states->size())
      , extras(impl.states->size())
    {}

    // Add s to l, following unlabeled arrows.
    // Next character to read is p.
    void addstate(List<Iter> *l, State const *s, boost::array<Sub<Iter>, NSUB> &m, Iter icur)
    {
        if(s == 0)
            return;

        StateEx<Iter> &ss = extras.stateex[s->id];
        if(ss.lastlist == extras.listid)
        {
            if(++ss.visits > 2)
                return;

            switch(matchtype)
            {
            case LeftmostBiased:
                if(reptype == RepeatMinimal || ++ss.visits > 2)
                    return;
                break;
            case LeftmostLongest:
                if(!longer(m, ss.lastthread->match))
                    return;
                break;
            }
        }
        else
        {
            ss.lastlist = extras.listid;
            ss.lastthread = &l->t[l->n++];
            ss.visits = 1;
        }

        if(ss.visits == 1)
        {
            ss.lastthread->state = s;
            ss.lastthread->match = m;
        }

        switch(s->op)
        {
        case Split:
            // follow unlabeled arrows
            addstate(l, s->out, m, icur);
            addstate(l, s->out1, m, icur);
            break;

        case LParen:
        {   // record left paren location and keep going
            Sub<Iter> save = m[s->data];
            m[s->data].first = icur;
            m[s->data].matched = Incomplete;
            addstate(l, s->out, m, icur);
            // restore old information before returning.
            m[s->data] = save;
        }   break;

        case RParen:
        {   // record right paren location and keep going
            Sub<Iter> save = m[s->data];
            m[s->data].second = icur;
            m[s->data].matched = Matched;
            addstate(l, s->out, m, icur);
            // restore old information before returning.
            m[s->data] = save;
        }   break;

        default:
            break;
        }
    }

    // Step the NFA from the states in clist
    // past the character c,
    // to create next NFA state set nlist.
    // Record best match so far in *this.
    void
    step(List<Iter> *clist, int c, Iter icur, List<Iter> *nlist)
    {
        if(debug)
        {
            dumplist(clist, impl.nparen);
            std::cout << (char)c << " (" << c << ")\n";
        }

        ++extras.listid;
        nlist->n = 0;

        for(int i = 0; i < clist->n; ++i)
        {
            Thread<Iter> *t = &clist->t[i];

            if(matchtype == LeftmostLongest)
            {
                // stop any threads that are worse than the
                // leftmost longest found so far.  the threads
                // will end up ordered on the list by start point,
                // so if this one is too far right, all the rest are too.
                if(subs[0].matched != Unmatched && subs[0].first < t->match[0].first)
                {
                    break;
                }
            }

            switch(t->state->op)
            {
            case Char:
                if(c == t->state->data)
                {
                    addstate(nlist, t->state->out, t->match, icur);
                }
                break;

            case Any:
                addstate(nlist, t->state->out, t->match, icur);
                break;

            case Match:
                switch(matchtype)
                {
                case LeftmostBiased:
                    // best so far ...
                    subs = t->match;
                    // ... because we cut off the worse ones right now!
                    return;
                case LeftmostLongest:
                    if(longer(t->match, subs))
                    {
                        subs = t->match;
                    }
                    break;
                default:
                    break;
                }
                break;
            default:
                break;
            }
        }

        // start a new thread if no match yet 
        if(subs[0].matched == Unmatched)
        {
            addstate(nlist, impl.start, dummy, icur);
        }
    }

    // Compute initial thread list 
    List<Iter> *startlist(Iter icur, List<Iter> *l)
    {
        List<Iter> empty(0);
        std::fill_n(&subs[0], (int)NSUB, Sub<Iter>());
        step(&empty, 0, icur, l);
        return l;
    }

    bool match(Iter icur, Iter iend)
    {
        begin = icur; end = iend;
        List<Iter> *clist = startlist(icur, &l1);
        List<Iter> *nlist = &l2;

        for(; icur != end && clist->n > 0; ++icur)
        {
            int c = *icur & 0xFF;
            step(clist, c, boost::next(icur), nlist);
            std::swap(clist, nlist);
        }

        step(clist, 0, icur, nlist);
        return subs[0].matched == Matched;
    }

    void printmatch(boost::array<Sub<Iter>, NSUB> const &m, int n)
    {
        for(int i = 0; i < n; ++i)
        {
            if(m[i].matched == Matched)
                std::cout << '(' << std::distance(begin, m[i].first) << ',' << std::distance(begin, m[i].second) << ')';
            else if(m[i].matched == Incomplete)
                std::cout << '(' << std::distance(begin, m[i].first) << ",?)";
            else
                std::cout << "(?,?)";
        }
    }

    void dumplist(List<Iter> const *l, int nparen)
    {
        for(int i=0; i<l->n; ++i)
        {
            Thread<Iter> const *t = &l->t[i];
            if(t->state->op != Char && t->state->op != Any && t->state->op != Match)
            {
                continue;
            }
            std::cout << "  ";
            std::cout << t->state->id << ' ';
            printmatch(t->match, nparen+1);
            std::cout << '\n';
        }
    }

    REImpl const &impl;
    Iter begin, end;
    boost::array<Sub<Iter>, NSUB> subs, dummy;
    List<Iter> l1, l2;
    Extras<Iter> extras;
};

int main(int argc, char *argv[])
{
    for(;;)
    {
        if(argc > 1 && std::strcmp(argv[1], "-d") == 0)
        {
            debug++;
            argv[1] = argv[0]; --argc; ++argv;
        }
        else if(argc > 1 && std::strcmp(argv[1], "-l") == 0)
        {
            matchtype = LeftmostLongest;
            argv[1] = argv[0]; argc--; argv++;
        }
        else if(argc > 1 && std::strcmp(argv[1], "-p") == 0)
        {
            reptype = RepeatLikePerl;
            argv[1] = argv[0]; argc--; argv++;
        }
        else
        {
            break;
        }
    }

    if(argc < 3)
    {
        std::cerr << "USAGE: " << argv[0] << " <regexp> string...\n";
        return 1;
    }

    REImpl impl;
    regex_grammar<char const *> regex_parser;
    const char *begin = argv[1], *end = begin + std::strlen(begin);
    if (!qi::parse(begin, end, regex_parser(phoenix::ref(impl))) || begin != end)
    {
        std::cerr << "ERROR: invalid regex\n";
        return 1;
    }

    if(debug)
    {
        impl.dump();
    }

    Matcher<std::string::const_iterator> m(impl);
    for(int i=2; i<argc; ++i)
    {
        std::string str(argv[i]);
        if(m.match(str.begin(), str.end()))
        {
            std::cout << argv[i] << ": ";
            m.printmatch(m.subs, impl.nparen + 1);
            std::cout << '\n';
        }
    }
}

/*
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 */
