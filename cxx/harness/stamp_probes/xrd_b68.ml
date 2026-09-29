module type R = sig
    module X : sig module N : sig type s val x : s end end
    val f : X.N.s -> int
end
