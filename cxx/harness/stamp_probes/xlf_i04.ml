module M = struct
  module Fast : sig module R (D : sig end) : sig end val v : int end
  = struct module R (D : sig end) = struct end let v = 1 end
  let _ = Fast.v
end
