module type R = sig
    module X : sig module N : sig module M : sig type s val x : s end end end
    type u = X.N.M.s
end
