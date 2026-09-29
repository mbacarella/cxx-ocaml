module type S = sig
  module M : sig module N : sig type t type u end end
  type t
 end
let x = (module struct
    module M = struct module N = struct type t type u end end
    type t
  end : S)
