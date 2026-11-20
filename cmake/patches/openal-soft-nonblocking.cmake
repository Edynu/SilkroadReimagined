# Applied to the fetched OpenAL Soft source (cmake/AudioDependencies.cmake), in its source folder. Idempotent.
#
# 1. Makes its NONBLOCKING attribute macro empty, as it already is on compilers without [[clang::nonblocking]]: with
#    clang and the MSVC standard library the attribute on member functions breaks std::invoke's type traits (and its
#    -Werror=function-effects check rejects std::span).
file(READ common/opthelpers.h text)
string(REPLACE "#if HAS_ATTRIBUTE(clang::nonblocking)"
               "#if 0 /* SroReimagined: off, see cmake/patches/openal-soft-nonblocking.cmake */"
               text "${text}")
file(WRITE common/opthelpers.h "${text}")

# 2. MSVC 2026's standard library takes no pointer to a standard type's member function in std::views::transform
#    (alc.cpp's &EffectSlotCluster::operator*, a unique_ptr's): the same dereference as a lambda.
file(READ alc/alc.cpp text)
string(REPLACE "std::views::transform(&ContextBase::EffectSlotCluster::operator*)"
               "std::views::transform([](ContextBase::EffectSlotCluster &cluster) -> auto& { return *cluster; })"
               text "${text}")
file(WRITE alc/alc.cpp "${text}")
