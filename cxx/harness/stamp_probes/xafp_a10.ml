module M : sig type u val x : u val y : u -> u end = struct
  type u = int
  let x = 0
  let y v = v
end
let z = M.y M.x
