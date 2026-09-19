module A = struct module rec Bad : sig val f : int -> int end = struct let f =
  let y = 5 in fun x -> x+y end end
