let s = List.to_seq [1; 2]
let l = List.of_seq s
let a = Array.of_list l
let m = List.filter_map (fun x -> if x > 1 then Some x else None) l
