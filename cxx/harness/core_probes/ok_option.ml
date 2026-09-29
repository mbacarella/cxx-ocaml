let o = Some 3
let v = match o with Some x -> x | None -> 0
let f = Option.map (fun x -> x * 2)
let g = Option.value ~default:0
