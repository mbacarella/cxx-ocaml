module M : sig end = struct
  module Fast : sig module type S = sig type data end end
  = struct module type S = sig type data end end
  module type T = Fast.S
end
