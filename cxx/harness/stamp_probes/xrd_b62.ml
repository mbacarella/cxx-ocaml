module type R = sig
    module X : sig module N : sig type s val x : s end end
    module Y : sig module N : sig type s val x : s end end
    type u = X.N.s
    type w = Y.N.s
end
