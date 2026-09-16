module M : sig end = struct
  module Fast : sig module R (D : sig type data end) : sig end end
  = struct module R (D : sig type data end) = struct end end
end
