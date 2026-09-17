module A = struct module Make (M : sig end) = struct let f s = s end
include Make (struct end) end
