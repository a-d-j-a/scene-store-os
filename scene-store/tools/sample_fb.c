#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
int main(int argc, char **argv) {
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) return 1;
    char magic[3]={0}; uint32_t w,h; fscanf(fp,"%2s",magic); fscanf(fp," %u %u",&w,&h); uint32_t mv; fscanf(fp," %u",&mv); fgetc(fp);
    uint8_t *px = malloc(w*h*3); fread(px,3,w*h,fp); fclose(fp);
    int coords[][2] = {{512,384},{512,100},{512,750},{50,50},{960,50},{512,700}};
    const char *labels[] = {"center","top-mid","bottom-mid","top-left corner","top-right corner","above-panel"};
    for(int i=0;i<6;i++){
        int x=coords[i][0],y=coords[i][1];
        if(x>=(int)w||y>=(int)h)continue;
        uint8_t *p=px+(y*w+x)*3;
        fprintf(stdout,"%s(%d,%d): R=%3u G=%3u B=%3u\n",labels[i],x,y,p[0],p[1],p[2]);
    }
    /* Check if mostly colored or mostly black */
    uint64_t r_sum=0,g_sum=0,b_sum=0; uint32_t cnt=0;
    for(uint32_t y=0;y<h;y+=10){for(uint32_t x=0;x<w;x+=10){
        uint8_t *p=px+(y*w+x)*3; r_sum+=p[0]; g_sum+=p[1]; b_sum+=p[2]; cnt++;
    }}
    fprintf(stdout,"avg: R=%llu G=%llu B=%llu (over %u samples)\n",r_sum/cnt,g_sum/cnt,b_sum/cnt,cnt);
    free(px); return 0;
}
