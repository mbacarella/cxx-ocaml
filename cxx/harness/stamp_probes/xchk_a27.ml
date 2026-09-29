module type S = sig
  module M : sig type t type u end
  type t
 end
module X : S = struct
    module M = struct type t type u end
    type t
  end
let x = (module X : S)
