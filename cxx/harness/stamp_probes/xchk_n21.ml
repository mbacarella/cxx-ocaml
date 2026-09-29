module type S = sig
  module type T = sig module N : sig type t type u end end
 end
let x = (module struct
    module type T = sig module N : sig type t type u end end
  end : S)
