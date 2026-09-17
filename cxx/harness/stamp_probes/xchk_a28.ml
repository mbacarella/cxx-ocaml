module type S = sig
  module M : sig type t type u end
  type t
 end
let x = (module struct
    module M = struct type t type u end
    type t
  end : S)
