module W = struct module A = struct module Bad : sig val f : int end = struct
  let f = 1 end end end let z = 1
