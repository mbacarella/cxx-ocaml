module M : sig type make_dec val add_dec: make_dec -> unit end = struct
  module Fast = struct module type S = sig type data end end
  type make_dec
  let add_dec (dec : make_dec) = ignore dec
end
