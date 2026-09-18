module TT : sig module N : sig module M : sig val a : int end val x : int end
  end = struct module N = struct module M = struct let a = 1 end let x = 1 end
  end
