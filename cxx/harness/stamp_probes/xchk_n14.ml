module type S = sig
  module F : sig module G : sig module H : sig type t end end end
 end
let x = (module struct
    module F = struct module G = struct module H = struct type t end end end
  end : S)
