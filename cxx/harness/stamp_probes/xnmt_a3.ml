module type S = sig
  type u
  val mk : string -> u
  val un : u -> string
end
module A : S = struct
  type u = string
  let mk s = s
  let un s = s
end
let w = A.un (A.mk "")
