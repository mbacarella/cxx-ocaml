module M : sig end = struct
  module Fast : sig module type S = sig type data end end
  = struct module type S = sig type data end let x = 1 end
end
