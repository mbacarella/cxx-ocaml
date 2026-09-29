module M : sig
  val f : string -> string
  val g : string -> int
end = struct
  let f s = s
  let g = String.length
end
let a = M.f ""
let b = M.g ""
