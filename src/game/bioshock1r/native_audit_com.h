#pragma once
// Small owning reference used by the diagnostic and its offline fixture.
// No WRL dependency: the repo's minimal GPU-test SDK setup excludes WinRT.
namespace bvr::b1r::native_audit {
template<class T> class ComPtr {
    T* value_ = nullptr;
public:
    ComPtr() = default;
    explicit ComPtr(T* p) : value_(p) { if (value_) value_->AddRef(); }
    ComPtr(const ComPtr& p) : ComPtr(p.value_) {}
    ComPtr(ComPtr&& p) noexcept : value_(p.value_) { p.value_ = nullptr; }
    ~ComPtr() { Reset(); }
    ComPtr& operator=(T* p) {
        if (p) p->AddRef();
        Reset(); value_ = p; return *this;
    }
    ComPtr& operator=(const ComPtr& p) { return *this = p.value_; }
    ComPtr& operator=(ComPtr&& p) noexcept {
        if (this != &p) { Reset(); value_ = p.value_; p.value_ = nullptr; }
        return *this;
    }
    void Reset() { if (value_) { value_->Release(); value_ = nullptr; } }
    void Attach(T* p) { Reset(); value_ = p; }
    T* Get() const { return value_; }
    T** GetAddressOf() { return &value_; }
    T* operator->() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
};
} // namespace bvr::b1r::native_audit
