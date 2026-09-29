module type S = sig module type T end
module type U1 = S with module type T = S -> S
module type U2 = S with module type T := S -> S
module type U3 = functor (X : S with module type T = S) -> S
module type U4 = (S with module type T = S) -> S
module type U5 = S with module type T = S -> S with module type T = S
