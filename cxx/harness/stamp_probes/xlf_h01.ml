module M = struct
  module Fast : sig module type S = sig type data end end
  = struct module type S = sig type data end end
  type make_dec
  let add_dec (dec : make_dec) = ignore dec
end
