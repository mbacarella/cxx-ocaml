module type R = sig
    module X : sig module N : sig type s val x : s end end
    open X.N
    type u = s
end
