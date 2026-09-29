
module A = struct type t end
module B = A

module F (X : sig type t end) = X
module F' = F
module C = F'(A)

module C' = F(B)
module D = C

module G = B
include G

module type S = sig
  module M : sig val s : unit end
  module F : functor (S : sig end ) -> sig type t end
 end
let x = (module struct
    module M = struct let s = () end
    module F (_ : sig end) = struct type t end
  end : S)

module X = (val x)
module Y = X.M
