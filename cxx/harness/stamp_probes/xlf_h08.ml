module M : sig end = struct
  module Fast : sig module type S = sig type data val v : int end end
  = struct module type S = sig type data val v : int end end
end
