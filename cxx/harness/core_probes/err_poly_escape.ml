let bad (type a) (x : a) = x
let r = ref (bad 1)
let () = r := "s"
