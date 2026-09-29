module M : sig end = struct
  module Fast : sig module type S = sig type data end
  module type T = sig type d2 end end
  = struct module type S = sig type data end
  module type T = sig type d2 end end
end
