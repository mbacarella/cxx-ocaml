module M = struct
  module Fast : sig module type S end
  = struct module type S = sig type data end end
end
