# **LVGL Map Tile Converter**

This Python script is designed to convert a directory of PNG map tiles into a format compatible with LVGL (Light and Versatile Graphics Library) version 9\. The output is a series of binary files (.bin) with a header and pixel data in the RGB565 format, optimized for direct use with LVGL's image APIs.

The script is multithreaded and can significantly speed up the conversion process for large tile sets.

## **Features**

* **PNG to RGB565 Conversion**: Converts standard 24-bit PNG images into 16-bit RGB565.  
* **LVGL v9 Compatibility**: Generates a .bin file with the correct LVGL v9 header format.  
* **Multithreaded Conversion**: Utilizes a ThreadPoolExecutor to process tiles in parallel, configurable with the \--jobs flag.  
* **Directory Traversal**: Automatically finds and converts all .png files within a specified input directory structure.  
* **Skip Existing Files**: Skips conversion for tiles that already exist in the output directory unless the \--force flag is used.

## **Requirements**

The script requires the Pillow library to handle image processing. You can install it using pip:
```bash
pip install Pillow
```

## **Usage**

The script is a command-line tool. You can run it from your terminal with the following arguments:
```bash
python lvgl_map_tile_converter.py --input <input_folder> --output <output_folder> [options]
```

### **Arguments**

* \-i, \--input: **Required**. The root folder containing your map tiles. The script expects a tile structure like zoom/x/y.png.  
* \-o, \--output: **Required**. The root folder where the converted .bin tiles will be saved. The output structure will mirror the input: zoom/x/y.bin.  
* \-j, \--jobs: **Optional**. The number of worker threads to use for the conversion. Defaults to the number of CPU cores on your system. Using more jobs can speed up the process.  
* \-f, \--force: **Optional**. If this flag is set, the script will re-convert all tiles, even if the output .bin files already exist.

### **Examples**

**1\. Basic conversion with default settings:**
```bash
python lvgl_map_tile_converter.py --input ./map_tiles --output ./tiles1
```
**2\. Conversion using a specific number of threads (e.g., 8):**
```bash
python lvgl_map_tile_converter.py --input ./map_tiles --output ./tiles1 --jobs 8
```
**3\. Forcing a full re-conversion:**
```bash
python lvgl_map_tile_converter.py --input ./map_tiles --output ./tiles1 --force
```