let h = Hashtbl.create 16
let () = Hashtbl.add h 1 "a"
let v = Hashtbl.find_opt h 1
let b = Buffer.create 10
let () = Buffer.add_string b "x"
