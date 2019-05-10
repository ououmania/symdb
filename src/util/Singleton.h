# pragma once

namespace symutil
{

template <typename T>
class Singleton
{
public:
    static T& Instance()
    {
        static T obj;
        return obj;
    }

private:
};

} /* symutil */
