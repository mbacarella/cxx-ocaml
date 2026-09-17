module type T = sig module N : sig type s val x : s end end
module type R = sig
    module X : T
    type u = X.N.s
end
module type R2 = sig
    module X : T
    type u = X.N.s
end
