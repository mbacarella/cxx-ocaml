module M : sig module N : sig val x : int end end = struct module N = struct let x = "" end end
